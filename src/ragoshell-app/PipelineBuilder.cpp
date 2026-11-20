#include "ragoshell-app/PipelineBuilder.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <QPointer>

#include <bsoncxx/builder/basic/document.hpp>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/BsonUtils.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell-app/QmlBackend.h"
#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    namespace
    {
        constexpr int kPreviewDebounceMs = 500;
        constexpr int kPreviewMaxTimeMs = 10000;
        constexpr int kMaxSampleSize = 200;

        struct PreviewJob
        {
            quint64 generation = 0;
            std::shared_ptr<ConnectionSettings> connection;
            QString database;
            QString collection;
            std::vector<bsoncxx::document::value> stages;
            int limit = 20;
        };
    }

    // Single background thread; only the newest job is kept. Detached so a
    // slow preview never blocks closing the builder.
    struct PipelineBuilder::Worker
    {
        std::mutex mutex;
        std::condition_variable wake;
        bool stop = false;
        bool hasJob = false;
        PreviewJob job;

        static void run(std::shared_ptr<Worker> self, QPointer<PipelineBuilder> owner)
        {
            for (;;) {
                PreviewJob job;
                {
                    std::unique_lock<std::mutex> lock(self->mutex);
                    self->wake.wait(lock, [&self] { return self->stop || self->hasJob; });
                    if (self->stop)
                        return;
                    job = std::move(self->job);
                    self->hasJob = false;
                }
                auto result = std::make_shared<AggregationPipeline::PreviewResult>(
                    AggregationPipeline::runPreview(*job.connection, job.database, job.collection,
                                                    job.stages, job.limit, kPreviewMaxTimeMs));
                const quint64 generation = job.generation;
                QMetaObject::invokeMethod(owner, [owner, generation, result]() {
                    if (owner)
                        owner->applyPreview(generation, *result);
                }, Qt::QueuedConnection);
            }
        }
    };

    PipelineBuilder::PipelineBuilder(QObject *parent)
        : QAbstractListModel(parent),
          _worker(std::make_shared<Worker>())
    {
        _debounce.setSingleShot(true);
        _debounce.setInterval(kPreviewDebounceMs);
        connect(&_debounce, &QTimer::timeout, this, &PipelineBuilder::refreshPreview);
        std::thread(&Worker::run, _worker, QPointer<PipelineBuilder>(this)).detach();
    }

    PipelineBuilder::~PipelineBuilder()
    {
        {
            std::lock_guard<std::mutex> lock(_worker->mutex);
            _worker->stop = true;
        }
        _worker->wake.notify_all();
    }

    int PipelineBuilder::rowCount(const QModelIndex &parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(_stages.size());
    }

    QVariant PipelineBuilder::data(const QModelIndex &index, int role) const
    {
        if (!index.isValid() || index.row() >= static_cast<int>(_stages.size()))
            return QVariant();
        const PipelineStage &s = _stages[index.row()];
        switch (role) {
        case OperatorRole: return s.op;
        case BodyRole:     return s.body;
        case EnabledRole:  return s.enabled;
        case ErrorRole:    return _errors[index.row()];
        default:           return QVariant();
        }
    }

    QHash<int, QByteArray> PipelineBuilder::roleNames() const
    {
        return {
            {OperatorRole, "op"},
            {BodyRole, "body"},
            {EnabledRole, "stageEnabled"},
            {ErrorRole, "error"},
        };
    }

    void PipelineBuilder::setSelectedIndex(int index)
    {
        index = std::clamp(index, -1, static_cast<int>(_stages.size()) - 1);
        if (_selectedIndex == index)
            return;
        _selectedIndex = index;
        emit selectedIndexChanged();
        refreshPreview();
    }

    void PipelineBuilder::setSampleSize(int size)
    {
        size = std::clamp(size, 1, kMaxSampleSize);
        if (_sampleSize == size)
            return;
        _sampleSize = size;
        emit sampleSizeChanged();
        schedulePreview();
    }

    bool PipelineBuilder::configure(const QString &serverUuid, const QString &database,
                                    const QString &collection)
    {
        QmlBackend *backend = QmlSingletonInstance<QmlBackend>::peek();
        std::unique_ptr<ConnectionSettings> conn = backend ? backend->cloneConnection(serverUuid) : nullptr;
        if (!conn || database.isEmpty() || collection.isEmpty())
            return false;

        _connection = std::move(conn);
        _serverUuid = serverUuid;
        _database = database;
        _collection = collection;
        emit targetChanged();

        beginResetModel();
        _stages = {PipelineStage{QStringLiteral("$match"), AggregationPipeline::stageTemplate("$match"), true}};
        _errors = {QString()};
        endResetModel();
        emit countChanged();
        _selectedIndex = 0;
        emit selectedIndexChanged();
        refreshPreview();
        return true;
    }

    void PipelineBuilder::addStage(int afterIndex, const QString &op)
    {
        const int row = (afterIndex < 0 || afterIndex >= static_cast<int>(_stages.size()))
                            ? static_cast<int>(_stages.size())
                            : afterIndex + 1;
        beginInsertRows(QModelIndex(), row, row);
        _stages.insert(_stages.begin() + row, PipelineStage{op, AggregationPipeline::stageTemplate(op), true});
        _errors.insert(_errors.begin() + row, QString());
        endInsertRows();
        emit countChanged();
        validate(row);
        _selectedIndex = row;
        emit selectedIndexChanged();
        refreshPreview();
    }

    void PipelineBuilder::removeStage(int index)
    {
        if (index < 0 || index >= static_cast<int>(_stages.size()))
            return;
        beginRemoveRows(QModelIndex(), index, index);
        _stages.erase(_stages.begin() + index);
        _errors.erase(_errors.begin() + index);
        endRemoveRows();
        emit countChanged();
        if (_selectedIndex >= static_cast<int>(_stages.size()) || _selectedIndex > index) {
            _selectedIndex = std::max(-1, _selectedIndex - 1);
            emit selectedIndexChanged();
        }
        refreshPreview();
    }

    void PipelineBuilder::moveStage(int from, int to)
    {
        const int n = static_cast<int>(_stages.size());
        if (from < 0 || from >= n || to < 0 || to >= n || from == to)
            return;
        // QAbstractItemModel counts the destination before the move.
        beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to);
        PipelineStage stage = _stages[from];
        QString error = _errors[from];
        _stages.erase(_stages.begin() + from);
        _errors.erase(_errors.begin() + from);
        _stages.insert(_stages.begin() + to, stage);
        _errors.insert(_errors.begin() + to, error);
        endMoveRows();
        if (_selectedIndex == from) {
            _selectedIndex = to;
            emit selectedIndexChanged();
        }
        refreshPreview();
    }

    void PipelineBuilder::setOperator(int index, const QString &op)
    {
        if (index < 0 || index >= static_cast<int>(_stages.size()))
            return;
        PipelineStage &s = _stages[index];
        const QString trimmed = op.trimmed();
        if (s.op == trimmed)
            return;
        // Keep the user's body unless it is still the previous template.
        if (s.body.trimmed().isEmpty() || s.body == AggregationPipeline::stageTemplate(s.op))
            s.body = AggregationPipeline::stageTemplate(trimmed);
        s.op = trimmed;
        stagesChanged(index, index);
    }

    void PipelineBuilder::setBody(int index, const QString &body)
    {
        if (index < 0 || index >= static_cast<int>(_stages.size()) || _stages[index].body == body)
            return;
        _stages[index].body = body;
        stagesChanged(index, index);
    }

    void PipelineBuilder::setStageEnabled(int index, bool enabled)
    {
        if (index < 0 || index >= static_cast<int>(_stages.size()) || _stages[index].enabled == enabled)
            return;
        _stages[index].enabled = enabled;
        stagesChanged(index, index);
    }

    QString PipelineBuilder::pipelineScript() const
    {
        return AggregationPipeline::pipelineScript(_collection, _stages);
    }

    void PipelineBuilder::stagesChanged(int firstRow, int lastRow)
    {
        for (int i = firstRow; i <= lastRow; ++i)
            validate(i);
        emit dataChanged(index(firstRow), index(lastRow));
        // Edits after the previewed stage do not change its output.
        if (firstRow <= _selectedIndex)
            schedulePreview();
    }

    void PipelineBuilder::validate(int i)
    {
        bsoncxx::document::value parsed = bsoncxx::builder::basic::make_document();
        QString error;
        _errors[i] = AggregationPipeline::parseStage(_stages[i], parsed, error) ? QString() : error;
    }

    void PipelineBuilder::schedulePreview()
    {
        _debounce.start();
    }

    void PipelineBuilder::refreshPreview()
    {
        _debounce.stop();
        ++_generation;
        _previewText.clear();
        _previewCount = 0;
        _previewError.clear();

        if (!_connection || _selectedIndex < 0 || _selectedIndex >= static_cast<int>(_stages.size())) {
            _previewBusy = false;
            _previewTitle.clear();
            emit previewChanged();
            return;
        }

        const PipelineStage &target = _stages[_selectedIndex];
        _previewTitle = target.enabled
            ? tr("Output of stage %1 (%2)").arg(_selectedIndex + 1).arg(target.op)
            : tr("Stage %1 is disabled: output of the enabled stages before it").arg(_selectedIndex + 1);

        PreviewJob job;
        for (int i = 0; i <= _selectedIndex; ++i) {
            const PipelineStage &s = _stages[i];
            if (!s.enabled)
                continue;
            if (AggregationPipeline::isWriteStage(s.op)) {
                _previewError = tr("Stage %1 (%2) writes data and is not previewed.").arg(i + 1).arg(s.op);
                break;
            }
            bsoncxx::document::value parsed = bsoncxx::builder::basic::make_document();
            QString error;
            if (!AggregationPipeline::parseStage(s, parsed, error)) {
                _previewError = tr("Stage %1 (%2): %3").arg(i + 1).arg(s.op, error);
                break;
            }
            job.stages.push_back(std::move(parsed));
        }
        if (!_previewError.isEmpty()) {
            _previewBusy = false;
            emit previewChanged();
            return;
        }

        job.generation = _generation;
        job.connection = _connection;
        job.database = _database;
        job.collection = _collection;
        job.limit = _sampleSize;
        {
            std::lock_guard<std::mutex> lock(_worker->mutex);
            _worker->job = std::move(job);
            _worker->hasJob = true;
        }
        _worker->wake.notify_one();
        _previewBusy = true;
        emit previewChanged();
    }

    void PipelineBuilder::applyPreview(quint64 generation, const AggregationPipeline::PreviewResult &result)
    {
        if (generation != _generation)
            return;   // a newer preview is pending
        _previewBusy = false;
        if (!result.ok) {
            _previewError = result.error;
            emit previewChanged();
            return;
        }

        auto *settings = AppRegistry::instance().settingsManager();
        const UUIDEncoding uuidEncoding = settings ? settings->uuidEncoding() : DefaultEncoding;
        const SupportedTimes timeZone = settings ? settings->timeZone() : Utc;
        QStringList blocks;
        int n = 0;
        for (const auto &doc : result.documents) {
            blocks << QStringLiteral("/* %1 */\n").arg(++n) +
                      QtUtils::toQString(BsonUtils::jsonString(doc.view(), TenGen, 1, uuidEncoding, timeZone));
        }
        _previewCount = static_cast<int>(result.documents.size());
        _previewText = blocks.join(QStringLiteral("\n\n"));
        emit previewChanged();
    }
}

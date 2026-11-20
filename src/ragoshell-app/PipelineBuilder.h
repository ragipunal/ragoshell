#pragma once

#include <memory>
#include <vector>

#include <QAbstractListModel>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include "ragoshell/core/utils/AggregationPipeline.h"

namespace RagoShell
{
    class ConnectionSettings;

    /**
     * @brief QML "PipelineBuilder": editable aggregation stages (rows) plus a
     * live preview of the selected stage's output.
     *
     * Previews run pipeline[0..selected] + $limit on a worker thread with a
     * standalone client, so the shell tabs stay free. Only the newest preview
     * request is executed; stale results are dropped.
     */
    class PipelineBuilder : public QAbstractListModel
    {
        Q_OBJECT
        QML_ELEMENT

        Q_PROPERTY(QString serverUuid READ serverUuid NOTIFY targetChanged FINAL)
        Q_PROPERTY(QString database READ database NOTIFY targetChanged FINAL)
        Q_PROPERTY(QString collection READ collection NOTIFY targetChanged FINAL)
        Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)
        Q_PROPERTY(int selectedIndex READ selectedIndex WRITE setSelectedIndex NOTIFY selectedIndexChanged FINAL)
        Q_PROPERTY(int sampleSize READ sampleSize WRITE setSampleSize NOTIFY sampleSizeChanged FINAL)
        Q_PROPERTY(QStringList operators READ operators CONSTANT FINAL)
        Q_PROPERTY(bool previewBusy READ previewBusy NOTIFY previewChanged FINAL)
        Q_PROPERTY(QString previewTitle READ previewTitle NOTIFY previewChanged FINAL)
        Q_PROPERTY(QString previewText READ previewText NOTIFY previewChanged FINAL)
        Q_PROPERTY(int previewCount READ previewCount NOTIFY previewChanged FINAL)
        Q_PROPERTY(QString previewError READ previewError NOTIFY previewChanged FINAL)

    public:
        enum Roles {
            OperatorRole = Qt::UserRole + 1,
            BodyRole,
            EnabledRole,
            ErrorRole
        };

        explicit PipelineBuilder(QObject *parent = nullptr);
        ~PipelineBuilder() override;

        int rowCount(const QModelIndex &parent = QModelIndex()) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        QString serverUuid() const { return _serverUuid; }
        QString database() const { return _database; }
        QString collection() const { return _collection; }
        int selectedIndex() const { return _selectedIndex; }
        void setSelectedIndex(int index);
        int sampleSize() const { return _sampleSize; }
        void setSampleSize(int size);
        QStringList operators() const { return AggregationPipeline::commonOperators(); }
        bool previewBusy() const { return _previewBusy; }
        QString previewTitle() const { return _previewTitle; }
        QString previewText() const { return _previewText; }
        int previewCount() const { return _previewCount; }
        QString previewError() const { return _previewError; }

        /// Targets a connected server's collection and starts with one $match stage.
        Q_INVOKABLE bool configure(const QString &serverUuid, const QString &database,
                                   const QString &collection);
        /// Inserts a stage after 'afterIndex' (-1 = at the end) and selects it.
        Q_INVOKABLE void addStage(int afterIndex, const QString &op = QStringLiteral("$match"));
        Q_INVOKABLE void removeStage(int index);
        Q_INVOKABLE void moveStage(int from, int to);
        /// Changing the operator swaps in its template while the body is untouched.
        Q_INVOKABLE void setOperator(int index, const QString &op);
        Q_INVOKABLE void setBody(int index, const QString &body);
        Q_INVOKABLE void setStageEnabled(int index, bool enabled);
        Q_INVOKABLE QString pipelineScript() const;
        Q_INVOKABLE void refreshPreview();

    Q_SIGNALS:
        void targetChanged();
        void countChanged();
        void selectedIndexChanged();
        void sampleSizeChanged();
        void previewChanged();

    private:
        struct Worker;

        void stagesChanged(int firstRow, int lastRow);
        void validate(int index);
        void schedulePreview();
        void applyPreview(quint64 generation, const AggregationPipeline::PreviewResult &result);

        QString _serverUuid;
        QString _database;
        QString _collection;
        std::shared_ptr<ConnectionSettings> _connection;
        std::vector<PipelineStage> _stages;
        std::vector<QString> _errors;
        int _selectedIndex = -1;
        int _sampleSize = 20;

        bool _previewBusy = false;
        QString _previewTitle;
        QString _previewText;
        int _previewCount = 0;
        QString _previewError;
        quint64 _generation = 0;
        QTimer _debounce;
        std::shared_ptr<Worker> _worker;
    };
}

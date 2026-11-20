/*
    wdb - weather and water data storage

    Copyright (C) 2007 met.no

    Contact information:
    Norwegian Meteorological Institute
    Box 43 Blindern
    0313 OSLO
    NORWAY
    E-mail: wdb@met.no

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
    MA  02110-1301, USA
*/

// Rewritten for this project: boost::posix_time replaced with QDateTime
// (Qt is already a hard dependency of the application).

#include "ptimeutil.h"

#include <QDateTime>
#include <QString>

namespace miutil
{
    const long long minDate = -9218988800000; // "1677-11-10T17:46:40.001Z"
    const long long maxDate = 9218988800000;  // "2262-02-20T06:13:19.999Z"

    std::string isotimeString(long long millisSinceEpoch, bool useTseparator, bool isLocalFormat)
    {
        if (millisSinceEpoch <= minDate || millisSinceEpoch >= maxDate)
            return std::string();

        const char *fmt = useTseparator ? "yyyy-MM-dd'T'HH:mm:ss.zzz" : "yyyy-MM-dd HH:mm:ss.zzz";

        if (!isLocalFormat) {
            const QDateTime dt = QDateTime::fromMSecsSinceEpoch(millisSinceEpoch, Qt::UTC);
            if (!dt.isValid())
                return std::string();
            return (dt.toString(fmt) + 'Z').toStdString();
        }

        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(millisSinceEpoch);
        if (!dt.isValid())
            return std::string();

        QString out = dt.toString(fmt);
        const int offsetSec = dt.offsetFromUtc();
        const char sign = offsetSec < 0 ? '-' : '+';
        const int absSec = offsetSec < 0 ? -offsetSec : offsetSec;
        char buf[8];
        snprintf(buf, sizeof(buf), "%c%02d:%02d", sign, absSec / 3600, (absSec % 3600) / 60);
        out += buf;
        return out.toStdString();
    }

    long long msFromIsoString(const std::string &isoTime, bool &isSuccessfull)
    {
        isSuccessfull = true;
        const QString str = QString::fromStdString(isoTime).trimmed();

        if (str == "infinity")
            return maxDate;
        if (str == "-infinity")
            return minDate;
        if (str == "epoch")
            return 0;
        if (str == "now")
            return QDateTime::currentMSecsSinceEpoch();
        if (str == "today")
            return QDateTime::currentDateTimeUtc().date().startOfDay(Qt::UTC).toMSecsSinceEpoch();
        if (str == "tomorrow")
            return QDateTime::currentDateTimeUtc().date().addDays(1).startOfDay(Qt::UTC).toMSecsSinceEpoch();
        if (str == "yesterday")
            return QDateTime::currentDateTimeUtc().date().addDays(-1).startOfDay(Qt::UTC).toMSecsSinceEpoch();

        QDateTime dt = QDateTime::fromString(str, Qt::ISODateWithMs);
        if (!dt.isValid())
            dt = QDateTime::fromString(str, Qt::ISODate);
        if (!dt.isValid())
            dt = QDateTime::fromString(str, Qt::RFC2822Date);

        if (!dt.isValid()) {
            isSuccessfull = false;
            return 0;
        }

        return dt.toMSecsSinceEpoch();
    }
}

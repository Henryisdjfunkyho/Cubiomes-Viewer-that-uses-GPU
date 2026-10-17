#ifndef SEARCHRATEFORMAT_H
#define SEARCHRATEFORMAT_H

#include <QString>

namespace SearchRateFormat
{
inline QString format(double value)
{
    if (value >= 1.0e9)
        return QString::asprintf("%.1fG", value * 1.0e-9);
    if (value >= 1.0e6)
        return QString::asprintf("%.1fM", value * 1.0e-6);
    if (value >= 1.0e3)
        return QString::asprintf("%.1fK", value * 1.0e-3);
    return QString::asprintf("%.2f", value);
}
}

#endif

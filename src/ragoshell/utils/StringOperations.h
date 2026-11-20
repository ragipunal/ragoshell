#pragma once

#include <string>

namespace RagoShell
{
    // Capitalize first char (Mongo errors often come all lower case)
    std::string captilizeFirstChar(std::string str);
}

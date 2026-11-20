#include "ragoshell/core/engine/ShellBackend.h"

#include "ragoshell/core/engine/JsShellBackend.h"

namespace RagoShell
{
    std::unique_ptr<ShellBackend> createShellBackend()
    {
        return std::make_unique<JsShellBackend>();
    }
}

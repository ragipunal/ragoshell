#include "ragoshell/core/services/UiServices.h"

namespace RagoShell
{
    namespace
    {
        IUiServices *g_instance = nullptr;
    }

    namespace UiServices
    {
        IUiServices *instance()
        {
            return g_instance;
        }

        void setInstance(IUiServices *services)
        {
            g_instance = services;
        }
    }
}

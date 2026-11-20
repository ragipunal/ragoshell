#include "ragoshell/core/AppRegistry.h"

#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/domain/App.h"

namespace RagoShell
{
    AppRegistry::AppRegistry() :
        _bus(new EventBus()),
        _settingsManager(new SettingsManager()),
        _app(new App(_bus.get()))
    {
    }

    AppRegistry::~AppRegistry()
    {   
    }
}


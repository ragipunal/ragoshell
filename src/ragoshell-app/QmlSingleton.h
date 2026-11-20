#pragma once

#include <QJSEngine>
#include <QQmlEngine>

namespace RagoShell
{
    /**
     * @brief Exposes an object owned by main() as a QML_SINGLETON.
     *
     * Typed singletons (unlike context properties) let qmlsc compile the
     * bindings that use them. main() calls set() before loading QML; the
     * class's static create() factory returns get().
     */
    template <typename T>
    class QmlSingletonInstance
    {
    public:
        static void set(T *instance) { s_instance = instance; }

        /// The registered instance or nullptr; no ownership change.
        static T *peek() { return s_instance; }

        static T *get()
        {
            Q_ASSERT(s_instance);
            // main() owns it; the QML engine must never delete it.
            QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
            return s_instance;
        }

    private:
        static inline T *s_instance = nullptr;
    };
}

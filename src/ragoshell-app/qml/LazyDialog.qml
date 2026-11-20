import QtQuick

// Creates its dialog on first use instead of at startup (several dialogs
// embed Scintilla editors, each backed by a hidden QWidget). Call get()
// and invoke the dialog's open*() on the returned item.
Loader {
    active: false

    function get() {
        active = true
        return item
    }
}

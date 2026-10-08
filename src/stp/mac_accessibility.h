#pragma once

namespace Stp {
// Install after QApplication loads Cocoa and before any tables are created.
// Applies only to the pinned, unpatched Qt 6.8.3 backend.
bool installMacAccessibilityCompatibility();
}

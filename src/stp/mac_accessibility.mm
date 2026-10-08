#include "mac_accessibility.h"

#include <QAccessible>
#include <QAccessibleTableInterface>
#include <QtGui/private/qaccessiblecache_p.h>
#include <QDebug>
#include <AppKit/AppKit.h>
#include <objc/runtime.h>
#include <cstring>

// Qt 6.8.3 transfers a synthesized cell from its row array into the native
// accessibility cache without a cache reference. Interface deletion then
// releases the array's reference; draining that array causes a use-after-free.
// This narrowly adapts the ownership correction from Qt's QTBUG-134784 fix:
// https://github.com/qt/qtbase/commit/b1ed5f656f064e553b33752f8e87d2f5b9553e38
// The original 6.8.3 invalidate() releases its cache reference itself, so the
// newer QtGui ABI / invalidate implementation must not be mixed into this fix.

namespace {
using QueryInterface = QAccessibleInterface* (*)(id, SEL);
QueryInterface originalQuery = nullptr;
Ivar idVariable = nullptr, rowVariable = nullptr, columnVariable = nullptr, roleVariable = nullptr;

template<typename T> T value(id object, Ivar variable)
{
    T result;
    std::memcpy(&result, reinterpret_cast<const char*>(object) + ivar_getOffset(variable), sizeof(T));
    return result;
}
template<typename T> void assign(id object, Ivar variable, T newValue)
{
    std::memcpy(reinterpret_cast<char*>(object) + ivar_getOffset(variable), &newValue, sizeof(T));
}

QAccessibleInterface* queryWithOwnedCell(id element, SEL selector)
{
    NSString* role = value<NSString*>(element, roleVariable);
    if (role != NSAccessibilityCellRole) return originalQuery(element, selector);

    const auto tableId = value<QAccessible::Id>(element, idVariable);
    auto tableInterface = QAccessible::accessibleInterface(tableId);
    if (!tableInterface || !tableInterface->isValid()) return nullptr;
    auto table = tableInterface->tableInterface();
    if (!table) return nullptr;
    const int row = value<int>(element, rowVariable), column = value<int>(element, columnVariable);
    if (row < 0 || column < 0 || row >= table->rowCount() || column >= table->columnCount()) return nullptr;
    auto cell = table->cellAt(row, column);
    if (!cell || !cell->isValid()) return nullptr;

    const auto cellId = QAccessible::uniqueId(cell);
    auto cache = QAccessibleCache::instance();
    id previous = cache->elementForId(cellId);
    assign(element, idVariable, cellId);
    assign<NSString*>(element, roleVariable, nil);
    if (previous != element) {
        [element retain]; // independent of the row array's ownership
        cache->insertElement(cellId, reinterpret_cast<QMacAccessibilityElement*>(element));
        // Publish the replacement before notifying clients about the old cell.
        // The pinned backend's invalidate consumes exactly its old cache share.
        if (previous) [previous performSelector:sel_registerName("invalidate")];
    }
    return cell;
}
}

bool Stp::installMacAccessibilityCompatibility()
{
    static const bool installed = [] {
        if (std::strcmp(qVersion(), "6.8.3") != 0) return false;
        const Class nativeClass = objc_getClass("QMacAccessibilityElement");
        if (!nativeClass) return false;
        const SEL selector = sel_registerName("qtInterface");
        Method method = class_getInstanceMethod(nativeClass, selector);
        idVariable = class_getInstanceVariable(nativeClass, "axid");
        rowVariable = class_getInstanceVariable(nativeClass, "m_rowIndex");
        columnVariable = class_getInstanceVariable(nativeClass, "m_columnIndex");
        roleVariable = class_getInstanceVariable(nativeClass, "synthesizedRole");
        const auto scalarType = [](Ivar variable, const char* type) {
            return variable && std::strcmp(ivar_getTypeEncoding(variable), type) == 0;
        };
        if (!method || method_getNumberOfArguments(method) != 2
            || !scalarType(idVariable, @encode(QAccessible::Id))
            || !scalarType(rowVariable, @encode(int)) || !scalarType(columnVariable, @encode(int))
            || !roleVariable || ivar_getTypeEncoding(roleVariable)[0] != '@'
            || !class_getInstanceMethod(nativeClass, sel_registerName("invalidate"))) {
            qWarning() << "Qt Cocoa accessibility compatibility skipped: unknown backend ABI";
            return false;
        }
        originalQuery = reinterpret_cast<QueryInterface>(method_getImplementation(method));
        method_setImplementation(method, reinterpret_cast<IMP>(queryWithOwnedCell));
        return true;
    }();
    return installed;
}

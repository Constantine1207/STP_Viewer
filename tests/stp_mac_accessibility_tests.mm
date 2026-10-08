#include "mac_accessibility.h"
#include <QApplication>
#include <QAccessible>
#include <QtGui/private/qaccessiblecache_p.h>
#include <QTableWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTest>
#include <AppKit/AppKit.h>
#include <objc/runtime.h>
#include <cstdlib>
#include <iostream>

@interface NSObject (StpAccessibilityProbe)
+ (id)elementWithInterface:(QAccessibleInterface*)interface;
- (QAccessibleInterface*)qtInterface;
- (BOOL)isManagedByParent;
- (void)updateTableModel;
@end

// Exercise the app's own native provider inside the process, including delayed
// client references. No external process accessibility or global input APIs.
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (argc < 2) return 2;
    const bool baseline = argc > 2 && std::string(argv[2]) == "--no-fix";
    const bool installed = !baseline && Stp::installMacAccessibilityCompatibility();
    QJsonObject report{{"compatibilityInstalled", installed}, {"baselineWithoutFix", baseline}};
    auto save = [&](bool passed) {
        report["passed"] = passed;
        QSaveFile file(QString::fromLocal8Bit(argv[1]));
        if (!file.open(QIODevice::WriteOnly)) return false;
        file.write(QJsonDocument(report).toJson());
        return file.commit();
    };
    if (!baseline && !installed) { save(false); return 1; }
    const Class provider = objc_getClass("QMacAccessibilityElement");
    if (!provider) { save(false); return 1; }
    int promoted = 0, repeated = 0, tables = 0;
    @autoreleasepool {
        for (int cycle = 0; cycle < 100; ++cycle) {
            @autoreleasepool {
                QTableWidget table(3 + cycle % 8, 3 + cycle % 9);
                for (int row = 0; row < table.rowCount(); ++row)
                    for (int column = 0; column < table.columnCount(); ++column)
                        table.setItem(row, column, new QTableWidgetItem(QString::number(row * 10 + column)));
                auto interface = QAccessible::queryAccessibleInterface(&table);
                id native = [provider elementWithInterface:interface];
                NSMutableArray* held = [NSMutableArray array];
                for (int refresh = 0; refresh < 3; ++refresh) {
                    NSArray* rows = [native accessibilityRows];
                    [held addObject:rows];
                    for (id row in rows) {
                        NSArray* cells = [row accessibilityChildren];
                        if (!cells) continue;
                        [held addObject:cells];
                        for (id cell in cells) {
                            const bool placeholder = [cell isManagedByParent];
                            const auto before = [cell retainCount];
                            const auto actual = [cell qtInterface];
                            const auto after = [cell retainCount];
                            if (placeholder && actual) {
                                ++promoted;
                                if (after != before + 1) {
                                    report["promotionCacheReferenceBalanced"] = false;
                                    report["retainCountBefore"] = qint64(before);
                                    report["retainCountAfter"] = qint64(after);
                                    save(false);
                                    // Stop the known-bad baseline before draining
                                    // references whose ownership has failed.
                                    std::_Exit(1);
                                }
                            }
                            if (actual) {
                                [cell qtInterface];
                                if ([cell retainCount] != after) { save(false); return 1; }
                                ++repeated;
                            }
                        }
                    }
                    table.clear();
                    table.setRowCount(0);
                    table.setRowCount(3 + (cycle + refresh) % 8);
                    QApplication::processEvents();
                }
                ++tables;
                // table teardown invalidates the interface cache; held arrays
                // outlive the table and drain at the end of this pool.
            }
        }
        // Keep the model and accessible cell ID stable while rebuilding native
        // row wrappers: replacing the cache entry must invalidate and release
        // the old cell once, without consuming the held row array's ownership.
        @autoreleasepool {
            QTableWidget table(2, 2);
            for (int row = 0; row < 2; ++row)
                for (int column = 0; column < 2; ++column)
                    table.setItem(row, column, new QTableWidgetItem(QString::number(row + column)));
            id native = [provider elementWithInterface:QAccessible::queryAccessibleInterface(&table)];
            NSArray* oldRows = [[native accessibilityRows] retain];
            id oldCell = [[[oldRows objectAtIndex:0] accessibilityChildren] objectAtIndex:0];
            auto cellInterface = [oldCell qtInterface];
            const auto cellId = QAccessible::uniqueId(cellInterface);
            const auto before = [oldCell retainCount];
            [native updateTableModel];
            NSArray* newRows = [native accessibilityRows];
            id newCell = [[[newRows objectAtIndex:0] accessibilityChildren] objectAtIndex:0];
            auto replacement = [newCell qtInterface];
            const bool replaced = oldCell != newCell && replacement == cellInterface
                && QAccessibleCache::instance()->elementForId(cellId)
                    == reinterpret_cast<QMacAccessibilityElement*>(newCell)
                && [oldCell qtInterface] == nullptr && [oldCell retainCount] + 1 == before;
            report["sameCellIdReplacementBalanced"] = replaced;
            [oldRows release];
            if (!replaced) { save(false); return 1; }
        }
    }
    report["promotionCacheReferenceBalanced"] = promoted > 0;
    report["repeatedQueriesDoNotRetainAgain"] = repeated >= promoted;
    report["tableRefreshAndTeardownCycles"] = tables;
    report["promotedCells"] = promoted;
    report["repeatedCellQueries"] = repeated;
    const bool passed = installed && tables == 100 && promoted > 0 && repeated >= promoted
        && report["sameCellIdReplacementBalanced"].toBool();
    if (!save(passed)) return 2;
    std::cout << QJsonDocument(report).toJson().toStdString();
    return passed ? 0 : 1;
}

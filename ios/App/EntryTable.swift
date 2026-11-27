#if os(macOS)
import AppKit
import SwiftUI

/// A dictionary file's entries on the Mac, as an AppKit table: a page of 500 SwiftUI buttons (the English file's)
/// was slow to open and to move through with VoiceOver (Levi, 2026-11-27); a table makes only the rows it shows.
/// Moving through the rows only selects them (focusing a row never opens it); Return, a double-click or VO-Space
/// opens the selected entry. Each row's VoiceOver actions preview it.
struct EntryTable: NSViewRepresentable {
    struct Item: Equatable {
        let key: String
        let title: String
        let detail: String
        let canPreview: Bool
    }

    let items: [Item]
    var selectKey: String?  // the row to select (and focus) when the table appears: the entry opened last
    let open: (String) -> Void
    let preview: (String, DictionaryStore.PreviewScope) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSScrollView {
        let c = context.coordinator
        let table = OpeningTable()
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("entry"))
        column.title = "Entry"
        column.resizingMask = .autoresizingMask
        table.addTableColumn(column)
        table.headerView = nil
        table.style = .inset
        table.rowHeight = 38
        table.usesAlternatingRowBackgroundColors = true
        table.allowsMultipleSelection = false
        table.columnAutoresizingStyle = .uniformColumnAutoresizingStyle
        table.dataSource = c
        table.delegate = c
        table.target = c
        table.doubleAction = #selector(Coordinator.doubleClicked(_:))
        table.onReturn = { [weak c] in c?.openSelected() }
        table.setAccessibilityLabel("Entries")
        c.table = table
        c.items = items

        let scroll = NSScrollView()
        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.drawsBackground = false
        table.reloadData()
        if let k = selectKey, let row = items.firstIndex(where: { $0.key == k }) {
            table.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
            table.scrollRowToVisible(row)
            DispatchQueue.main.async { table.window?.makeFirstResponder(table) }  // back where the person was
        }
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        let c = context.coordinator
        c.parent = self
        guard c.items != items, let table = c.table else { return }
        let selected = table.selectedRow >= 0 && table.selectedRow < c.items.count ? c.items[table.selectedRow].key : nil
        c.items = items
        table.reloadData()
        if let selected, let row = items.firstIndex(where: { $0.key == selected }) {
            table.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
        }
    }

    final class Coordinator: NSObject, NSTableViewDataSource, NSTableViewDelegate {
        var parent: EntryTable
        var items: [Item] = []
        weak var table: NSTableView?

        init(_ parent: EntryTable) { self.parent = parent }

        func numberOfRows(in tableView: NSTableView) -> Int { items.count }

        func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
            let id = NSUserInterfaceItemIdentifier("entryCell")
            let cell = tableView.makeView(withIdentifier: id, owner: nil) as? EntryCell ?? EntryCell(id)
            cell.show(items[row])
            return cell
        }

        func tableView(_ tableView: NSTableView, rowViewForRow row: Int) -> NSTableRowView? {
            let rv = EntryRow()
            rv.coordinator = self
            rv.row = row
            return rv
        }

        @objc func doubleClicked(_ sender: NSTableView) {
            if sender.clickedRow >= 0 { open(sender.clickedRow) }
        }

        func openSelected() {
            if let r = table?.selectedRow, r >= 0 { open(r) }
        }

        func open(_ row: Int) {
            guard row >= 0, row < items.count else { return }
            parent.open(items[row].key)
        }

        func actions(for row: Int) -> [NSAccessibilityCustomAction] {
            guard row >= 0, row < items.count, items[row].canPreview else { return [] }
            let key = items[row].key
            return [("Preview", DictionaryStore.PreviewScope.alone), ("Preview with Earlier Entries", .earlier),
                    ("Preview with All Entries", .all)].map { name, scope in
                NSAccessibilityCustomAction(name: name) { [weak self] in
                    self?.parent.preview(key, scope)
                    return true
                }
            }
        }
    }

    /// Return and Enter open the selected row (the table's own keys otherwise: arrows, type to select).
    final class OpeningTable: NSTableView {
        var onReturn: (() -> Void)?

        override func keyDown(with event: NSEvent) {
            if event.keyCode == 36 || event.keyCode == 76, event.modifierFlags.intersection([.command, .option, .control, .shift]).isEmpty {
                onReturn?()
            } else {
                super.keyDown(with: event)
            }
        }
    }

    /// A row: VoiceOver's press (VO-Space) opens it, as the buttons did; its actions preview it.
    final class EntryRow: NSTableRowView {
        weak var coordinator: Coordinator?
        var row = -1

        override func accessibilityPerformPress() -> Bool {
            coordinator?.open(row)
            return true
        }

        override func accessibilityCustomActions() -> [NSAccessibilityCustomAction]? {
            coordinator?.actions(for: row)
        }
    }

    /// The pattern, with the replacement, type and whose it is under it.
    final class EntryCell: NSTableCellView {
        private let title = NSTextField(labelWithString: "")
        private let detail = NSTextField(labelWithString: "")

        init(_ id: NSUserInterfaceItemIdentifier) {
            super.init(frame: .zero)
            identifier = id
            for f in [title, detail] {
                f.lineBreakMode = .byTruncatingTail
                f.maximumNumberOfLines = 1
                f.cell?.truncatesLastVisibleLine = true
                f.translatesAutoresizingMaskIntoConstraints = false
                f.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
                addSubview(f)
            }
            detail.font = .preferredFont(forTextStyle: .footnote)
            detail.textColor = .secondaryLabelColor
            textField = title
            NSLayoutConstraint.activate([
                title.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
                title.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -4),
                title.topAnchor.constraint(equalTo: topAnchor, constant: 2),
                detail.leadingAnchor.constraint(equalTo: title.leadingAnchor),
                detail.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -4),
                detail.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 1),
            ])
        }

        required init?(coder: NSCoder) { fatalError("not from a nib") }

        func show(_ item: Item) {
            title.stringValue = item.title
            detail.stringValue = item.detail
            // read as the buttons were: pattern, then the rest
            setAccessibilityLabel("\(item.title), \(item.detail)")
        }
    }
}
#endif

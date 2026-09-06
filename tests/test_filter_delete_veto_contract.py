from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def section(source: str, start: str, end: str) -> str:
    start_index = source.index(start)
    return source[start_index : source.index(end, start_index + len(start))]


class FilterDeleteVetoContractTests(unittest.TestCase):
    def test_delete_is_a_two_phase_vetoable_decorator_contract(self) -> None:
        interface = read("Editor/IFilterGUI.h")
        comment_h = read("Editor/guis/CommentFilterGUI.h")
        comment_cpp = read("Editor/guis/CommentFilterGUI.cpp")
        audio_h = read("Editor/guis/AudioToolFilterGUIFactory.h")
        audio_cpp = read("Editor/guis/AudioToolFilterGUIFactory.cpp")
        vst_h = read("Editor/guis/VSTPluginFilterGUI.h")
        vst_cpp = read("Editor/guis/VSTPluginFilterGUI.cpp")

        self.assertIn("virtual bool prepareDelete() { return true; }", interface)
        self.assertIn("virtual bool commitDelete() { return true; }", interface)
        self.assertIn("bool prepareDelete() override;", comment_h)
        self.assertIn("return child->prepareDelete();", comment_cpp)
        self.assertIn("bool commitDelete() override;", comment_h)
        self.assertIn("return child->commitDelete();", comment_cpp)
        self.assertIn("bool commitDelete() override;", audio_h)
        audio_delete = section(
            audio_cpp,
            "bool AudioToolFilterGUI::commitDelete()",
            "void AudioToolFilterGUI::destroyMeterDialog()",
        )
        self.assertLess(
            audio_delete.index("destroyMeterDialog();"),
            audio_delete.index("return true;"),
        )
        self.assertIn("bool prepareDelete() override;", vst_h)
        vst_delete = section(
            vst_cpp,
            "bool VSTPluginFilterGUI::prepareDelete()",
            "bool VSTPluginFilterGUI::commitDelete()",
        )
        for side_effect in (
            "ensureOutProcPanelStopped",
            "terminateOutProcPanel",
            "OutProcReadVSTConfig",
            "QFile::remove",
        ):
            self.assertNotIn(side_effect, vst_delete)
        self.assertIn("canRemoveExistingFile(outProcGuiConfigPath)", vst_delete)
        vst_commit = section(
            vst_cpp,
            "bool VSTPluginFilterGUI::commitDelete()",
            "void VSTPluginFilterGUI::on_openPanelButton_clicked()",
        )
        self.assertIn(
            "return !outProcMode || ensureOutProcPanelStopped(true);", vst_commit
        )
        ensure_stopped = section(
            vst_cpp,
            "bool VSTPluginFilterGUI::ensureOutProcPanelStopped",
            "void VSTPluginFilterGUI::applyDialog",
        )
        self.assertLess(
            ensure_stopped.index('ui->openPanelButton->setText(tr("Open panel"))'),
            ensure_stopped.index("emit updateModel();"),
        )

    def test_table_replacement_and_single_row_removal_preflight_before_mutation(
        self,
    ) -> None:
        table = read("Editor/FilterTable.cpp")
        set_lines = section(
            table,
            "bool FilterTable::setLines",
            "FilterTable::Item* FilterTable::addLine",
        )
        self.assertLess(
            set_lines.index("prepareDeleteAllItems()"),
            set_lines.index("this->configPath = configPath;"),
        )
        self.assertLess(
            set_lines.index("!commitDeleteItems(items)"),
            set_lines.index("setLinesAfterDeleteCommit(configPath, lines);"),
        )
        self.assertLess(
            set_lines.index("prepareDeleteAllItems()"),
            set_lines.index("qDeleteAll(items);"),
        )

        replace = section(
            table,
            "bool FilterTable::replaceItemText",
            "bool FilterTable::removeItem",
        )
        self.assertLess(
            replace.index("prepareItemReplacement(item)"),
            replace.index("commitDeleteItem(item)"),
        )
        self.assertLess(
            replace.index("commitDeleteItem(item)"),
            replace.index("item->text = text;"),
        )

        remove = section(
            table,
            "bool FilterTable::removeItem",
            "bool FilterTable::prepareDeleteItem",
        )
        self.assertLess(
            remove.index("prepareDeleteItem(item)"),
            remove.index("commitDeleteItem(item)"),
        )
        self.assertLess(
            remove.index("commitDeleteItem(item)"),
            remove.index("items.removeOne(item);"),
        )
        self.assertIn("return false;", remove)

    def test_multi_delete_is_all_or_nothing_for_list_and_selection(self) -> None:
        table = read("Editor/FilterTable.cpp")
        delete_selected = section(
            table,
            "bool FilterTable::deleteSelectedLines",
            "void FilterTable::deletePreparedItems",
        )
        delete_prepared = section(
            table,
            "void FilterTable::deletePreparedItems",
            "void FilterTable::selectAll",
        )
        preflight = delete_selected.index("prepareDeleteItems(itemsToDelete)")
        commit = delete_selected.index("!commitDeleteItems(itemsToDelete)")
        self.assertLess(preflight, commit)
        self.assertLess(
            commit, delete_selected.index("deletePreparedItems(itemsToDelete);")
        )
        self.assertIn(
            "return false;",
            delete_selected[
                : delete_selected.index("deletePreparedItems(itemsToDelete);")
            ],
        )
        self.assertIn("focused = NULL;", delete_prepared)
        self.assertIn("delete item;", delete_prepared)
        self.assertIn("selected.clear();", delete_prepared)

    def test_cut_preflights_before_payload_and_deletes_same_prepared_items(
        self,
    ) -> None:
        table = read("Editor/FilterTable.cpp")
        cut = section(table, "void FilterTable::cut()", "void FilterTable::copy()")
        self.assertLess(
            cut.index("prepareDeleteItems(itemsToCut)"),
            cut.index("commitDeleteItems(itemsToCut)"),
        )
        self.assertLess(
            cut.index("commitDeleteItems(itemsToCut)"),
            cut.index("copyItemsToClipboard(itemsToCut)"),
        )
        self.assertLess(
            cut.index("copyItemsToClipboard(itemsToCut)"),
            cut.index("deletePreparedItems(itemsToCut)"),
        )
        self.assertEqual(cut.count("prepareDeleteItems(itemsToCut)"), 1)
        copy_helper = section(
            table,
            "void FilterTable::populateMimeDataFromItems",
            "void FilterTable::copyItemsToClipboard",
        )
        self.assertLess(
            copy_helper.index("item->gui->storePreferences(item->prefs);"),
            copy_helper.index("prefsList.append(item->prefs);"),
        )

    def test_copy_insertion_regenerates_shared_instance_ids_but_moves_preserve_them(
        self,
    ) -> None:
        table = read("Editor/FilterTable.cpp")
        clone = section(
            table,
            "QString cloneFilterLineWithNewInstanceIds",
            "class ThemeIconLabel",
        )
        paste = section(
            table, "void FilterTable::paste()", "bool FilterTable::deleteSelectedLines"
        )
        drop = section(
            table, "void FilterTable::dropEvent", "void FilterTable::keyPressEvent"
        )
        copy = section(
            table,
            "void FilterTable::copy()",
            "QList<FilterTable::Item*> FilterTable::selectedItemsInOrder",
        )
        cut = section(table, "void FilterTable::cut()", "void FilterTable::copy()")

        self.assertIn("HostId", clone)
        self.assertIn("MeterId", clone)
        self.assertGreaterEqual(clone.count("QUuid::createUuid()"), 2)
        self.assertIn("tokenizeFilterParameters(parameters)", clone)
        self.assertIn("VSTParseLegacyParameterIndex", clone)
        self.assertNotIn("QRegularExpression", clone)
        self.assertIn("keyIndex + 1 < tokens.size()", table)
        self.assertIn('clonedText.insert(separator + 1, " HostId "', clone)
        self.assertIn('clonedText.insert(separator + 1, " MeterId "', clone)
        self.assertIn("copyItemsToClipboard(selectedItemsInOrder());", copy)
        self.assertIn("copyItemsToClipboard(itemsToCut);", cut)
        self.assertIn("cloneFilterLineWithNewInstanceIds(textLines[i])", paste)
        self.assertIn("event->dropAction() == Qt::CopyAction", drop)
        self.assertIn("cloneFilterLineWithNewInstanceIds(line)", drop)

    def test_gui_rebuild_transfers_runtime_tracking_without_persisting_or_copying_it(
        self,
    ) -> None:
        interface = read("Editor/IFilterGUI.h")
        table_h = read("Editor/FilterTable.h")
        table = read("Editor/FilterTable.cpp")
        comment = read("Editor/guis/CommentFilterGUI.cpp")
        vst = read("Editor/guis/VSTPluginFilterGUI.cpp")
        main = read("Editor/MainWindow.cpp")

        self.assertIn("virtual void restoreRuntimeState", interface)
        self.assertIn("virtual void takeRuntimeState", interface)
        self.assertIn("QVariantMap runtimeState;", table_h)
        update = section(
            table,
            "void FilterTable::updateGuis()",
            "void FilterTable::propagateChannels",
        )
        self.assertLess(
            update.index("takeRuntimeState"), update.index("object->deleteLater()")
        )
        self.assertLess(
            update.index("gui->loadPreferences"),
            update.index("gui->restoreRuntimeState"),
        )
        self.assertIn("item->runtimeState.clear();", update)
        self.assertIn("child->restoreRuntimeState(state);", comment)
        self.assertIn("child->takeRuntimeState(state);", comment)
        self.assertIn("if (!outProcRuntimeStateTransferred)", vst)
        self.assertIn("outProcConfigPath", vst)
        self.assertIn("outProcProcessCreationTime", vst)
        take_runtime = section(
            vst,
            "void VSTPluginFilterGUI::takeRuntimeState",
            "bool VSTPluginFilterGUI::prepareDelete",
        )
        self.assertLess(
            take_runtime.index("idleTimer.stop();"), take_runtime.index("state.insert")
        )

        snapshot = section(
            main,
            "bool MainWindow::loadSnapshotScenario",
            "bool MainWindow::snapshotLayoutIsValid",
        )
        self.assertIn("oldVstGui->restoreRuntimeState(expectedRuntimeState);", snapshot)
        self.assertIn("filterTable->updateGuis();", snapshot)
        self.assertIn("QEvent::DeferredDelete", snapshot)
        self.assertIn("rebuiltVstGui->takeRuntimeState(actualRuntimeState);", snapshot)
        self.assertIn("actualRuntimeState != expectedRuntimeState", snapshot)
        self.assertIn("HostId foo!", snapshot)
        self.assertIn("HostId foo?", snapshot)
        self.assertIn("firstCanonicalId == secondCanonicalId", snapshot)

    def test_drag_move_preflights_without_committing_until_move_is_accepted(
        self,
    ) -> None:
        table = read("Editor/FilterTable.cpp")
        drag = section(
            table,
            "void FilterTable::mouseMoveEvent",
            "void FilterTable::dragEnterEvent",
        )
        self.assertIn("if (!prepareDeleteItems(itemsToDelete))", drag)
        preflight = drag.index("if (!prepareDeleteItems(itemsToDelete))")
        self.assertLess(preflight, drag.index("QDrag* drag"))
        self.assertLess(preflight, drag.index("drag->exec("))
        self.assertLess(preflight, drag.index("FilterTableMimeData* mimeData"))
        self.assertIn(
            "return;", drag[preflight : drag.index("FilterTableMimeData* mimeData")]
        )
        self.assertLess(drag.index("drag->exec("), drag.index("mimeData->commitMove()"))
        self.assertLess(
            drag.index("mimeData->commitMove()"),
            drag.index("deletePreparedItems(itemsToDelete)"),
        )
        self.assertIn("setMoveCommitHandler", drag)
        self.assertIn("populateMimeDataFromItems(mimeData, itemsToDelete);", drag)

        drop = section(
            table, "void FilterTable::dropEvent", "void FilterTable::keyPressEvent"
        )
        commit = drop.index("!filterTableMimeData->commitMove()")
        self.assertLess(commit, drop.index("QString text = mimeData->text();"))
        self.assertLess(commit, drop.index("items.insert(dropRow++, item);"))
        self.assertIn("!cloneInstanceIds && filterTableMimeData != NULL", drop)

    def test_raw_text_edit_and_table_lifetime_entries_honor_veto(self) -> None:
        row = read("Editor/FilterTableRow.cpp")
        begin_edit = section(
            row,
            "void FilterTableRow::on_actionEditText_triggered",
            "void FilterTableRow::on_lineEdit_editingFinished()",
        )
        self.assertLess(
            begin_edit.index("table->prepareItemReplacement(item)"),
            begin_edit.index("ui->lineEdit->setText(item->text);"),
        )
        edit = section(
            row,
            "void FilterTableRow::on_lineEdit_editingFinished()",
            "void FilterTableRow::on_lineEdit_editingCanceled()",
        )
        self.assertIn("if (table->replaceItemText(item, ui->lineEdit->text()))", edit)
        self.assertIn("ui->lineEdit->setText(item->text);", edit)

        main = read("Editor/MainWindow.cpp")
        close_event = section(
            main, "void MainWindow::closeEvent", "void MainWindow::deviceSelected"
        )
        tab_close = section(
            main,
            "bool MainWindow::on_tabWidget_tabCloseRequested",
            "void MainWindow::on_actionOpen_triggered",
        )
        self.assertIn("!filterTable->prepareDeleteAllItems()", close_event)
        self.assertLess(
            close_event.index("!filterTable->prepareDeleteAllItems()"),
            close_event.index("askForClose(i)"),
        )
        self.assertLess(
            close_event.index("askForClose(i)"),
            close_event.index("restoreTemporaryProcessingState()"),
        )
        self.assertLess(
            close_event.index("restoreTemporaryProcessingState()"),
            close_event.index("!filterTable->commitDeleteAllItems()"),
        )
        self.assertLess(
            tab_close.index("!closingFilterTable->prepareDeleteAllItems()"),
            tab_close.index("askForClose(index)"),
        )
        self.assertLess(
            tab_close.index("askForClose(index)"),
            tab_close.index("restoreTemporaryProcessingState()"),
        )
        self.assertLess(
            tab_close.index("restoreTemporaryProcessingState()"),
            tab_close.index("!closingFilterTable->commitDeleteAllItems()"),
        )
        self.assertLess(
            tab_close.index("!closingFilterTable->commitDeleteAllItems()"),
            tab_close.index("ui->tabWidget->removeTab(index);"),
        )
        self.assertLess(
            tab_close.index("ui->tabWidget->removeTab(index);"),
            tab_close.index("closingScrollArea->deleteLater();"),
        )
        self.assertIn("qDeleteAll(items);", read("Editor/FilterTable.cpp"))

    def test_temporary_file_swaps_preflight_then_commit_after_successful_write(
        self,
    ) -> None:
        main = read("Editor/MainWindow.cpp")
        main_h = read("Editor/MainWindow.h")
        set_temporary = section(
            main,
            "bool MainWindow::setTemporaryLines",
            "bool MainWindow::restoreTemporaryLines",
        )
        self.assertLess(
            set_temporary.index("if (!filterTable->prepareDeleteAllItems())"),
            set_temporary.index("conditionallyWriteConfiguration("),
        )
        self.assertLess(
            set_temporary.index("conditionallyWriteConfiguration("),
            set_temporary.index("filterTable->commitDeleteAllItems()"),
        )
        self.assertLess(
            set_temporary.index("filterTable->commitDeleteAllItems()"),
            set_temporary.index("filterTable->setLinesAfterDeleteCommit(path, lines);"),
        )
        self.assertIn("rollbackConditionalConfigurationWrite(", set_temporary)
        self.assertIn("recoveredStateChanged", set_temporary)
        self.assertIn("suppressInstantSave", main_h)
        self.assertLess(
            set_temporary.index("instantSaveGuard(suppressInstantSave, true)"),
            set_temporary.index("filterTable->commitDeleteAllItems()"),
        )
        lines_changed = section(
            main, "void MainWindow::linesChanged", "void MainWindow::"
        )
        self.assertIn("!suppressInstantSave", lines_changed)

        restore = section(
            main,
            "bool MainWindow::restoreTemporaryLines",
            "bool MainWindow::writeTemporaryRecoveryJournal",
        )
        self.assertLess(
            restore.index("!filterTable->prepareDeleteAllItems()"),
            restore.index("conditionallyWriteConfiguration("),
        )
        self.assertIn("if (!filterTable->setLines(path, lines))", restore)

    def test_comparison_and_bypass_preflight_before_snapshot_or_dirty_gate(
        self,
    ) -> None:
        main = read("Editor/MainWindow.cpp")
        calibration = section(
            main,
            "bool MainWindow::beginTemporaryFilterConfiguration",
            "bool MainWindow::restoreTemporaryFilterConfiguration",
        )
        calibration_preflight = calibration.index(
            "filterTable->prepareDeleteAllItems()"
        )
        self.assertLess(
            calibration_preflight, calibration.index("isFilterTableDirty(filterTable)")
        )
        self.assertLess(
            calibration_preflight,
            calibration.index("originalLines = filterTable->getLines()"),
        )
        self.assertLess(
            calibration_preflight, calibration.index("writeTemporaryRecoveryJournal(")
        )

        capture = section(
            main,
            "void MainWindow::captureComparisonA()",
            "void MainWindow::comparisonToggled",
        )
        capture_preflight = capture.index("filterTable->prepareDeleteAllItems()")
        self.assertLess(
            capture_preflight, capture.index("isFilterTableDirty(filterTable)")
        )
        self.assertLess(
            capture_preflight,
            capture.index("comparisonALines = filterTable->getLines()"),
        )

        comparison = section(
            main,
            "void MainWindow::comparisonToggled",
            "void MainWindow::bypassToggled",
        )
        comparison_preflight = comparison.index("filterTable->prepareDeleteAllItems()")
        self.assertLess(
            comparison_preflight, comparison.index("isFilterTableDirty(filterTable)")
        )
        self.assertLess(
            comparison_preflight,
            comparison.index("comparisonBLines = filterTable->getLines()"),
        )
        self.assertLess(
            comparison_preflight, comparison.index("writeTemporaryRecoveryJournal(")
        )

        bypass = section(
            main,
            "void MainWindow::bypassToggled",
            "bool MainWindow::restoreTemporaryProcessingState",
        )
        bypass_preflight = bypass.index("filterTable->prepareDeleteAllItems()")
        self.assertLess(
            bypass_preflight, bypass.index("isFilterTableDirty(filterTable)")
        )
        self.assertLess(
            bypass_preflight,
            bypass.index("bypassOriginalLines = filterTable->getLines()"),
        )
        self.assertLess(
            bypass_preflight, bypass.index("writeTemporaryRecoveryJournal(")
        )


if __name__ == "__main__":
    unittest.main()

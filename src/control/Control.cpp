#include "Control.h"

#include "FullscreenHandler.h"
#include "LatexController.h"
#include "PageBackgroundChangeController.h"
#include "PrintHandler.h"
#include "UndoRedoController.h"
#include "layer/LayerController.h"

#include "gui/XournalppCursor.h"

#include "gui/TextEditor.h"
#include "gui/XournalView.h"
#include "gui/dialog/AboutDialog.h"
#include "gui/dialog/FillTransparencyDialog.h"
#include "gui/dialog/FormatDialog.h"
#include "gui/dialog/GotoDialog.h"
#include "gui/dialog/PageTemplateDialog.h"
#include "gui/dialog/SelectBackgroundColorDialog.h"
#include "gui/dialog/SettingsDialog.h"
#include "gui/dialog/ToolbarManageDialog.h"
#include "gui/dialog/toolbarCustomize/ToolbarDragDropHandler.h"
#include "gui/inputdevices/HandRecognition.h"
#include "gui/toolbarMenubar/ToolMenuHandler.h"
#include "gui/toolbarMenubar/model/ToolbarData.h"
#include "gui/toolbarMenubar/model/ToolbarModel.h"
#include "jobs/AutosaveJob.h"
#include "jobs/BlockingJob.h"
#include "jobs/CustomExportJob.h"
#include "jobs/PdfExportJob.h"
#include "jobs/SaveJob.h"
#include "model/BackgroundImage.h"
#include "model/FormatDefinitions.h"
#include "model/StrokeStyle.h"
#include "model/XojPage.h"
#include "pagetype/PageTypeHandler.h"
#include "pagetype/PageTypeMenu.h"
#include "plugin/PluginController.h"
#include "settings/ButtonConfig.h"
#include "stockdlg/XojOpenDlg.h"
#include "undo/AddUndoAction.h"
#include "undo/DeleteUndoAction.h"
#include "undo/InsertDeletePageUndoAction.h"
#include "undo/InsertUndoAction.h"
#include "view/DocumentView.h"
#include "view/TextView.h"
#include "xojfile/LoadHandler.h"

#include "CrashHandler.h"
#include "PathUtil.h"
#include "Stacktrace.h"
#include "StringUtils.h"
#include "Util.h"
#include "XojMsgBox.h"
#include "config-dev.h"
#include "config-features.h"
#include "config.h"
#include "i18n.h"
#include "serializing/ObjectInputStream.h"

#include "util/cpp14memory.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include <ctime>
#include <fstream>
#include <memory>
#include <numeric>
#include <sstream>


Control::Control(GladeSearchpath* gladeSearchPath)
{
	XOJ_INIT_TYPE(Control);

	this->recent = new RecentManager();
	this->undoRedo = new UndoRedoHandler(this);
	this->recent->addListener(this);
	this->undoRedo->addUndoRedoListener(this);
	this->isBlocking = false;

	this->gladeSearchPath = gladeSearchPath;

	this->metadata = new MetadataManager();
	this->cursor = new XournalppCursor(this);

	this->lastAction = ACTION_NONE;
	this->lastGroup = GROUP_NOGROUP;
	this->lastEnabled = false;

	Path name = Path(g_get_home_dir());
	name /= CONFIG_DIR;
	name /= SETTINGS_XML_FILE;
	this->settings = new Settings(name);
	this->settings->load();

	TextView::setDpi(settings->getDisplayDpi());

	this->pageTypes = new PageTypeHandler(gladeSearchPath);
	this->newPageType = new PageTypeMenu(this->pageTypes, settings, true, true);

	this->audioController = new AudioController(this->settings, this);

	this->scrollHandler = new ScrollHandler(this);

	this->scheduler = new XournalScheduler();

	this->doc = new Document(this);

	// for crashhandling
	setEmergencyDocument(this->doc);

	this->zoom = new ZoomControl();
	this->zoom->setZoomStep(this->settings->getZoomStep() / 100.0);
	this->zoom->setZoomStepScroll(this->settings->getZoomStepScroll() / 100.0);
	this->zoom->setZoom100Value(this->settings->getDisplayDpi() / 72.0);

	this->toolHandler = new ToolHandler(this, this, this->settings);
	this->toolHandler->loadSettings();

	/**
	 * This is needed to update the previews
	 */
	this->changeTimout = g_timeout_add_seconds(5, (GSourceFunc) checkChangedDocument, this);

	this->pageBackgroundChangeController = new PageBackgroundChangeController(this);

	this->layerController = new LayerController(this);
	this->layerController->registerListener(this);

	this->fullscreenHandler = new FullscreenHandler(settings);

	this->pluginController = new PluginController(this);
	this->pluginController->registerToolbar();
}

Control::~Control()
{
	XOJ_CHECK_TYPE(Control);

	g_source_remove(this->changeTimout);
	this->enableAutosave(false);

	deleteLastAutosaveFile("");

	this->scheduler->stop();

	for (XojPage* page: this->changedPages)
	{
		page->unreference();
	}

	delete this->pluginController;
	this->pluginController = nullptr;
	delete this->clipboardHandler;
	this->clipboardHandler = nullptr;
	delete this->recent;
	this->recent = nullptr;
	delete this->undoRedo;
	this->undoRedo = nullptr;
	delete this->settings;
	this->settings = nullptr;
	delete this->toolHandler;
	this->toolHandler = nullptr;
	delete this->sidebar;
	this->sidebar = nullptr;
	delete this->doc;
	this->doc = nullptr;
	delete this->searchBar;
	this->searchBar = nullptr;
	delete this->scrollHandler;
	this->scrollHandler = nullptr;
	delete this->newPageType;
	this->newPageType = nullptr;
	delete this->pageTypes;
	this->pageTypes = nullptr;
	delete this->metadata;
	this->metadata = nullptr;
	delete this->cursor;
	this->cursor = nullptr;
	delete this->zoom;
	this->zoom = nullptr;
	delete this->scheduler;
	this->scheduler = nullptr;
	delete this->dragDropHandler;
	this->dragDropHandler = nullptr;
	delete this->audioController;
	this->audioController = nullptr;
	delete this->pageBackgroundChangeController;
	this->pageBackgroundChangeController = nullptr;
	delete this->layerController;
	this->layerController = nullptr;
	delete this->fullscreenHandler;
	this->fullscreenHandler = nullptr;

	XOJ_RELEASE_TYPE(Control);
}

void Control::renameLastAutosaveFile()
{
	XOJ_CHECK_TYPE(Control);

	if (this->lastAutosaveFilename.isEmpty())
	{
		return;
	}

	Path filename = this->lastAutosaveFilename;
	Path renamed = Util::getAutosaveFilename();
	renamed.clearExtensions();
	if (filename.str().find_first_of(".") != 0)
	{
		// This file must be a fresh, unsaved document. Since this file is
		// already in ~/.xournalpp/autosave/, we need to change the renamed filename.
		renamed += ".old.autosave.xopp";
	}
	else
	{
		// The file is a saved document with the form ".<filename>.autosave.xopp"
		renamed += filename.getFilename();
	}

	g_message("%s",
	          FS(_F("Autosave renamed from {1} to {2}") % this->lastAutosaveFilename.str() % renamed.str()).c_str());

	if (!filename.exists())
	{
		this->save(false);
	}

	std::vector<string> errors;

	// See https://github.com/xournalpp/xournalpp/issues/1122 for why we use
	// `g_file_copy` instead of `g_rename` here.
	GFile* src = g_file_new_for_path(filename.c_str());
	GFile* dest = g_file_new_for_path(renamed.c_str());
	GError* err = nullptr;
	// Use target default perms; the source partition may have different file
	// system attributes than the target, and we don't want anything bad in the
	// autosave directory
	auto flags = static_cast<GFileCopyFlags>(G_FILE_COPY_TARGET_DEFAULT_PERMS | G_FILE_COPY_OVERWRITE);
	g_file_copy(src, dest, flags, nullptr, nullptr, nullptr, &err);
	if (err == nullptr)
	{
		g_file_delete(src, nullptr, &err);
	}
	g_object_unref(src);
	g_object_unref(dest);

	if (err != nullptr)
	{
		auto fmtstr = _F("Could not rename autosave file from \"{1}\" to \"{2}\": {3}");
		errors.push_back(FS(fmtstr % filename.str() % renamed.str() % err->message));
		g_error_free(err);
	}

	if (!errors.empty())
	{
		string error = std::accumulate(errors.begin() + 1, errors.end(), *errors.begin(),
		                               [](string e1, string e2) { return e1 + "\n" + e2; });
		Util::execInUiThread([=]() {
			string msg = FS(_F("Autosave failed with an error: {1}") % error);
			XojMsgBox::showErrorToUser(getGtkWindow(), msg);
		});
	}
}

void Control::setLastAutosaveFile(Path newAutosaveFile)
{
	this->lastAutosaveFilename = newAutosaveFile;
}

void Control::deleteLastAutosaveFile(Path newAutosaveFile)
{
	XOJ_CHECK_TYPE(Control);

	if (!this->lastAutosaveFilename.isEmpty())
	{
		// delete old autosave file
		g_unlink(this->lastAutosaveFilename.c_str());
	}
	this->lastAutosaveFilename = newAutosaveFile;
}

bool Control::checkChangedDocument(Control* control)
{
	XOJ_CHECK_TYPE_OBJ(control, Control);

	if (!control->doc->tryLock())
	{
		// call again later
		return true;
	}
	for (XojPage* page: control->changedPages)
	{
		int p = control->doc->indexOf(page);
		if (p != -1)
		{
			control->firePageChanged(p);
		}

		page->unreference();
	}
	control->changedPages.clear();

	control->doc->unlock();

	// Call again
	return true;
}

void Control::saveSettings()
{
	XOJ_CHECK_TYPE(Control);

	this->toolHandler->saveSettings();

	gint width = 0;
	gint height = 0;
	gtk_window_get_size(getGtkWindow(), &width, &height);

	if (!this->win->isMaximized())
	{
		this->settings->setMainWndSize(width, height);
	}
	this->settings->setMainWndMaximized(this->win->isMaximized());

	this->sidebar->saveSize();
}

void Control::initWindow(MainWindow* win)
{
	XOJ_CHECK_TYPE(Control);

	win->setRecentMenu(recent->getMenu());
	selectTool(toolHandler->getToolType());
	this->win = win;
	this->zoom->initZoomHandler(win->getXournal()->getWidget(), win->getXournal(), this);
	this->sidebar = new Sidebar(win, this);

	XojMsgBox::setDefaultWindow(getGtkWindow());

	updatePageNumbers(0, npos);

	toolHandler->eraserTypeChanged();

	this->searchBar = new SearchBar(this);

	// Disable undo buttons
	undoRedoChanged();

	if (settings->isPresentationMode())
	{
		setViewPresentationMode(true);
	}
	else if (settings->isViewFixedRows())
	{
		setViewRows(settings->getViewRows());
	}
	else
	{
		setViewColumns(settings->getViewColumns());
	}

	setViewLayoutVert(settings->getViewLayoutVert());
	setViewLayoutR2L(settings->getViewLayoutR2L());
	setViewLayoutB2T(settings->getViewLayoutB2T());

	setViewPairedPages(settings->isShowPairedPages());

	penSizeChanged();
	eraserSizeChanged();
	hilighterSizeChanged();
	updateDeletePageButton();
	toolFillChanged();
	toolLineStyleChanged();

	this->clipboardHandler = new ClipboardHandler(this, win->getXournal()->getWidget());

	this->enableAutosave(settings->isAutosaveEnabled());

	win->setFontButtonFont(settings->getFont());

	this->pluginController->registerMenu();

	fireActionSelected(GROUP_SNAPPING, settings->isSnapRotation() ? ACTION_ROTATION_SNAPPING : ACTION_NONE);
	fireActionSelected(GROUP_GRID_SNAPPING, settings->isSnapGrid() ? ACTION_GRID_SNAPPING : ACTION_NONE);
}

bool Control::autosaveCallback(Control* control)
{
	XOJ_CHECK_TYPE_OBJ(control, Control);

	if (!control->undoRedo->isChangedAutosave())
	{
		// do nothing, nothing changed
		return true;
	}
	else
	{
		g_message("Info: autosave document...");
	}

	AutosaveJob* job = new AutosaveJob(control);
	control->scheduler->addJob(job, JOB_PRIORITY_NONE);
	job->unref();

	return true;
}

void Control::enableAutosave(bool enable)
{
	XOJ_CHECK_TYPE(Control);

	if (this->autosaveTimeout)
	{
		g_source_remove(this->autosaveTimeout);
		this->autosaveTimeout = 0;
	}

	if (enable)
	{
		int timeout = settings->getAutosaveTimeout() * 60;
		this->autosaveTimeout = g_timeout_add_seconds(timeout, (GSourceFunc) autosaveCallback, this);
	}
}

void Control::updatePageNumbers(size_t page, size_t pdfPage)
{
	XOJ_CHECK_TYPE(Control);

	if (this->win == nullptr)
	{
		return;
	}

	this->win->updatePageNumbers(page, this->doc->getPageCount(), pdfPage);
	this->sidebar->selectPageNr(page, pdfPage);

	this->metadata->storeMetadata(this->doc->getEvMetadataFilename().str(), page, getZoomControl()->getZoomReal());

	int current = getCurrentPageNo();
	int count = this->doc->getPageCount();

	fireEnableAction(ACTION_GOTO_FIRST, current != 0);
	fireEnableAction(ACTION_GOTO_BACK, current != 0);
	fireEnableAction(ACTION_GOTO_PREVIOUS_ANNOTATED_PAGE, current != 0);

	fireEnableAction(ACTION_GOTO_PAGE, count > 1);

	fireEnableAction(ACTION_GOTO_NEXT, current < count - 1);
	fireEnableAction(ACTION_GOTO_LAST, current < count - 1);
	fireEnableAction(ACTION_GOTO_NEXT_ANNOTATED_PAGE, current < count - 1);
}

void Control::actionPerformed(ActionType type, ActionGroup group, GdkEvent* event, GtkMenuItem* menuitem,
                              GtkToolButton* toolbutton, bool enabled)
{
	XOJ_CHECK_TYPE(Control);

	if (layerController->actionPerformed(type))
	{
		return;
	}

	switch (type)
	{
		// Menu File
	case ACTION_NEW:
		clearSelectionEndText();
		newFile();
		break;
	case ACTION_OPEN:
		openFile();
		break;
	case ACTION_ANNOTATE_PDF:
		clearSelectionEndText();
		annotatePdf("", false, false);
		break;
	case ACTION_SAVE:
		save();
		break;
	case ACTION_SAVE_AS:
		saveAs();
		break;
	case ACTION_EXPORT_AS_PDF:
		exportAsPdf();
		break;
	case ACTION_EXPORT_AS:
		exportAs();
		break;
	case ACTION_PRINT:
		print();
		break;
	case ACTION_QUIT:
		quit();
		break;
		// Menu Edit
	case ACTION_UNDO:
		UndoRedoController::undo(this);
		break;
	case ACTION_REDO:
		UndoRedoController::redo(this);
		break;
	case ACTION_CUT:
		cut();
		break;
	case ACTION_COPY:
		copy();
		break;
	case ACTION_PASTE:
		paste();
		break;
	case ACTION_SEARCH:
		clearSelectionEndText();
		searchBar->showSearchBar(true);
		break;
	case ACTION_DELETE:
		if (!win->getXournal()->actionDelete())
		{
			deleteSelection();
		}
		break;
	case ACTION_SETTINGS:
		showSettings();
		break;

		// Menu Navigation
	case ACTION_GOTO_FIRST:
		scrollHandler->scrollToPage(0);
		break;
	case ACTION_GOTO_BACK:
		scrollHandler->goToPreviousPage();
		break;
	case ACTION_GOTO_PAGE:
		gotoPage();
		break;
	case ACTION_GOTO_NEXT:
		scrollHandler->goToNextPage();
		break;
	case ACTION_GOTO_LAST:
		scrollHandler->scrollToPage(this->doc->getPageCount() - 1);
		break;
	case ACTION_GOTO_NEXT_ANNOTATED_PAGE:
		scrollHandler->scrollToAnnotatedPage(true);
		break;
	case ACTION_GOTO_PREVIOUS_ANNOTATED_PAGE:
		scrollHandler->scrollToAnnotatedPage(false);
		break;

		// Menu Journal
	case ACTION_NEW_PAGE_BEFORE:
		insertNewPage(getCurrentPageNo());
		break;
	case ACTION_NEW_PAGE_AFTER:
		insertNewPage(getCurrentPageNo() + 1);
		break;
	case ACTION_NEW_PAGE_AT_END:
		insertNewPage(this->doc->getPageCount());
		break;
	case ACTION_DELETE_PAGE:
		deletePage();
		break;
	case ACTION_PAPER_FORMAT:
		paperFormat();
		break;
	case ACTION_CONFIGURE_PAGE_TEMPLATE:
		paperTemplate();
		break;
	case ACTION_PAPER_BACKGROUND_COLOR:
		changePageBackgroundColor();
		break;

		// Menu Tools
	case ACTION_TOOL_PEN:
		clearSelection();
		if (enabled)
		{
			selectTool(TOOL_PEN);
		}
		break;
	case ACTION_TOOL_ERASER:
		clearSelection();
		if (enabled)
		{
			selectTool(TOOL_ERASER);
		}
		break;

	case ACTION_TOOL_ERASER_STANDARD:
		if (enabled)
		{
			toolHandler->setEraserType(ERASER_TYPE_DEFAULT);
		}
		break;
	case ACTION_TOOL_ERASER_DELETE_STROKE:
		if (enabled)
		{
			toolHandler->setEraserType(ERASER_TYPE_DELETE_STROKE);
		}
		break;
	case ACTION_TOOL_ERASER_WHITEOUT:
		if (enabled)
		{
			toolHandler->setEraserType(ERASER_TYPE_WHITEOUT);
		}
		break;

	case ACTION_TOOL_HILIGHTER:
		clearSelection();
		if (enabled)
		{
			selectTool(TOOL_HILIGHTER);
		}
		break;
	case ACTION_TOOL_TEXT:
		clearSelection();
		if (enabled)
		{
			selectTool(TOOL_TEXT);
		}
		break;
	case ACTION_TOOL_IMAGE:
		clearSelection();
		if (enabled)
		{
			selectTool(TOOL_IMAGE);
		}
		break;
	case ACTION_TOOL_SELECT_RECT:
		if (enabled)
		{
			selectTool(TOOL_SELECT_RECT);
		}
		break;
	case ACTION_TOOL_SELECT_REGION:
		if (enabled)
		{
			selectTool(TOOL_SELECT_REGION);
		}
		break;
	case ACTION_TOOL_SELECT_OBJECT:
		if (enabled)
		{
			selectTool(TOOL_SELECT_OBJECT);
		}
		break;
	case ACTION_TOOL_PLAY_OBJECT:
		if (enabled)
		{
			selectTool(TOOL_PLAY_OBJECT);
		}
		break;
	case ACTION_TOOL_VERTICAL_SPACE:
		clearSelection();
		if (enabled)
		{
			selectTool(TOOL_VERTICAL_SPACE);
		}
		break;

	case ACTION_TOOL_HAND:
		if (enabled)
		{
			selectTool(TOOL_HAND);
		}
		break;
	case ACTION_TOOL_FLOATING_TOOLBOX:
		if (enabled)
		{
			selectTool(TOOL_FLOATING_TOOLBOX);
		}
		break;
	case ACTION_TOOL_DRAW_RECT:
	case ACTION_TOOL_DRAW_CIRCLE:
	case ACTION_TOOL_DRAW_ARROW:
	case ACTION_TOOL_DRAW_COORDINATE_SYSTEM:
	case ACTION_RULER:
	case ACTION_SHAPE_RECOGNIZER:
		setShapeTool(type, enabled);
		break;

	case ACTION_TOOL_DEFAULT:
		if (enabled)
		{
			selectDefaultTool();
		}
		break;
	case ACTION_TOOL_FILL:
		setFill(enabled);
		break;

	case ACTION_SIZE_VERY_THIN:
		if (enabled)
		{
			setToolSize(TOOL_SIZE_VERY_FINE);
		}
		break;
	case ACTION_SIZE_FINE:
		if (enabled)
		{
			setToolSize(TOOL_SIZE_FINE);
		}
		break;
	case ACTION_SIZE_MEDIUM:
		if (enabled)
		{
			setToolSize(TOOL_SIZE_MEDIUM);
		}
		break;
	case ACTION_SIZE_THICK:
		if (enabled)
		{
			setToolSize(TOOL_SIZE_THICK);
		}
		break;
	case ACTION_SIZE_VERY_THICK:
		if (enabled)
		{
			setToolSize(TOOL_SIZE_VERY_THICK);
		}
		break;

	case ACTION_TOOL_LINE_STYLE_PLAIN:
		setLineStyle("plain");
		break;
	case ACTION_TOOL_LINE_STYLE_DASH:
		setLineStyle("dash");
		break;
	case ACTION_TOOL_LINE_STYLE_DASH_DOT:
		setLineStyle("dashdot");
		break;
	case ACTION_TOOL_LINE_STYLE_DOT:
		setLineStyle("dot");
		break;

	case ACTION_TOOL_ERASER_SIZE_FINE:
		if (enabled)
		{
			this->toolHandler->setEraserSize(TOOL_SIZE_FINE);
			eraserSizeChanged();
		}
		break;
	case ACTION_TOOL_ERASER_SIZE_MEDIUM:
		if (enabled)
		{
			this->toolHandler->setEraserSize(TOOL_SIZE_MEDIUM);
			eraserSizeChanged();
		}
		break;
	case ACTION_TOOL_ERASER_SIZE_THICK:
		if (enabled)
		{
			this->toolHandler->setEraserSize(TOOL_SIZE_THICK);
			eraserSizeChanged();
		}
		break;
	case ACTION_TOOL_PEN_SIZE_VERY_THIN:
		if (enabled)
		{
			this->toolHandler->setPenSize(TOOL_SIZE_VERY_FINE);
			penSizeChanged();
		}
		break;
	case ACTION_TOOL_PEN_SIZE_FINE:
		if (enabled)
		{
			this->toolHandler->setPenSize(TOOL_SIZE_FINE);
			penSizeChanged();
		}
		break;
	case ACTION_TOOL_PEN_SIZE_MEDIUM:
		if (enabled)
		{
			this->toolHandler->setPenSize(TOOL_SIZE_MEDIUM);
			penSizeChanged();
		}
		break;
	case ACTION_TOOL_PEN_SIZE_THICK:
		if (enabled)
		{
			this->toolHandler->setPenSize(TOOL_SIZE_THICK);
			penSizeChanged();
		}
		break;
	case ACTION_TOOL_PEN_SIZE_VERY_THICK:
		if (enabled)
		{
			this->toolHandler->setPenSize(TOOL_SIZE_VERY_THICK);
			penSizeChanged();
		}
		break;
	case ACTION_TOOL_PEN_FILL:
		this->toolHandler->setPenFillEnabled(enabled);
		break;
	case ACTION_TOOL_PEN_FILL_TRANSPARENCY:
		selectFillAlpha(true);
		break;


	case ACTION_TOOL_HILIGHTER_SIZE_FINE:
		if (enabled)
		{
			this->toolHandler->setHilighterSize(TOOL_SIZE_FINE);
			hilighterSizeChanged();
		}
		break;
	case ACTION_TOOL_HILIGHTER_SIZE_MEDIUM:
		if (enabled)
		{
			this->toolHandler->setHilighterSize(TOOL_SIZE_MEDIUM);
			hilighterSizeChanged();
		}
		break;
	case ACTION_TOOL_HILIGHTER_SIZE_THICK:
		if (enabled)
		{
			this->toolHandler->setHilighterSize(TOOL_SIZE_THICK);
			hilighterSizeChanged();
		}
		break;
	case ACTION_TOOL_HILIGHTER_FILL:
		this->toolHandler->setHilighterFillEnabled(enabled);
		break;
	case ACTION_TOOL_HILIGHTER_FILL_TRANSPARENCY:
		selectFillAlpha(false);
		break;

	case ACTION_FONT_BUTTON_CHANGED:
		fontChanged();
		break;

	case ACTION_SELECT_FONT:
		if (win)
		{
			win->getToolMenuHandler()->showFontSelectionDlg();
		}
		break;

		// Used for all colors
	case ACTION_SELECT_COLOR:
	case ACTION_SELECT_COLOR_CUSTOM:
		// nothing to do here, the color toolbar item handles the color
		break;
	case ACTION_TEX:
		runLatex();
		break;

		// Menu View
	case ACTION_ZOOM_100:
	case ACTION_ZOOM_FIT:
	case ACTION_ZOOM_IN:
	case ACTION_ZOOM_OUT:
		Util::execInUiThread([=]() { zoomCallback(type, enabled); });
		break;

	case ACTION_VIEW_PAIRED_PAGES:
		setViewPairedPages(enabled);
		break;

	case ACTION_VIEW_PRESENTATION_MODE:
		setViewPresentationMode(enabled);
		break;

	case ACTION_MANAGE_TOOLBAR:
		manageToolbars();
		break;

	case ACTION_CUSTOMIZE_TOOLBAR:
		customizeToolbars();
		break;

	case ACTION_FULLSCREEN:
		setFullscreen(enabled);
		break;

	case ACTION_SET_COLUMNS_1:
		setViewColumns(1);
		break;

	case ACTION_SET_COLUMNS_2:
		setViewColumns(2);
		break;

	case ACTION_SET_COLUMNS_3:
		setViewColumns(3);
		break;

	case ACTION_SET_COLUMNS_4:
		setViewColumns(4);
		break;

	case ACTION_SET_COLUMNS_5:
		setViewColumns(5);
		break;

	case ACTION_SET_COLUMNS_6:
		setViewColumns(6);
		break;

	case ACTION_SET_COLUMNS_7:
		setViewColumns(7);
		break;

	case ACTION_SET_COLUMNS_8:
		setViewColumns(8);
		break;

	case ACTION_SET_ROWS_1:
		setViewRows(1);
		break;

	case ACTION_SET_ROWS_2:
		setViewRows(2);
		break;

	case ACTION_SET_ROWS_3:
		setViewRows(3);
		break;

	case ACTION_SET_ROWS_4:
		setViewRows(4);
		break;

	case ACTION_SET_ROWS_5:
		setViewRows(5);
		break;

	case ACTION_SET_ROWS_6:
		setViewRows(6);
		break;

	case ACTION_SET_ROWS_7:
		setViewRows(7);
		break;

	case ACTION_SET_ROWS_8:
		setViewRows(8);
		break;

	case ACTION_SET_LAYOUT_HORIZONTAL:
		setViewLayoutVert(false);
		break;

	case ACTION_SET_LAYOUT_VERTICAL:
		setViewLayoutVert(true);
		break;

	case ACTION_SET_LAYOUT_L2R:
		setViewLayoutR2L(false);
		break;

	case ACTION_SET_LAYOUT_R2L:
		setViewLayoutR2L(true);
		break;

	case ACTION_SET_LAYOUT_T2B:
		setViewLayoutB2T(false);
		break;

	case ACTION_SET_LAYOUT_B2T:
		setViewLayoutB2T(true);
		break;


	case ACTION_AUDIO_RECORD:
	{
		bool result;
		if (enabled)
		{
			result = audioController->startRecording();
		}
		else
		{
			result = audioController->stopRecording();
		}

		if (!result)
		{
			Util::execInUiThread([=]() {
				gtk_toggle_tool_button_set_active((GtkToggleToolButton*) toolbutton, !enabled);
				string msg = _("Recorder could not be started.");
				g_warning("%s", msg.c_str());
				XojMsgBox::showErrorToUser(Control::getGtkWindow(), msg);
			});
		}
		break;
	}

	case ACTION_AUDIO_PAUSE_PLAYBACK:
		if (enabled)
		{
			this->getAudioController()->pausePlayback();
		}
		else
		{
			this->getAudioController()->continuePlayback();
		}
		break;

	case ACTION_AUDIO_STOP_PLAYBACK:
		this->getAudioController()->stopPlayback();
		break;

	case ACTION_ROTATION_SNAPPING:
		rotationSnappingToggle();
		break;

	case ACTION_GRID_SNAPPING:
		gridSnappingToggle();
		break;

		// Footer, not really an action, but need an identifier to
	case ACTION_FOOTER_PAGESPIN:
	case ACTION_FOOTER_ZOOM_SLIDER:
		// nothing to do here
		break;


		// Plugin menu
	case ACTION_PLUGIN_MANAGER:
		this->pluginController->showPluginManager();
		break;


		// Menu Help
	case ACTION_HELP:
		XojMsgBox::showHelp(getGtkWindow());
		break;
	case ACTION_ABOUT:
		showAbout();
		break;

	default:
		g_warning("Unhandled action event: %s / %s (%i / %i)",
		          ActionType_toString(type).c_str(),
		          ActionGroup_toString(group).c_str(),
		          type,
		          group);
		Stacktrace::printStracktrace();
	}

	if (type >= ACTION_TOOL_PEN && type <= ACTION_TOOL_HAND)
	{
		ActionType at = (ActionType)(toolHandler->getToolType() - TOOL_PEN + ACTION_TOOL_PEN);
		if (type == at && !enabled)
		{
			fireActionSelected(GROUP_TOOL, at);
		}
	}
}

bool Control::copy()
{
	if (this->win && this->win->getXournal()->copy())
	{
		return true;
	}
	return this->clipboardHandler->copy();
}

bool Control::cut()
{
	if (this->win && this->win->getXournal()->cut())
	{
		return true;
	}
	return this->clipboardHandler->cut();
}

bool Control::paste()
{
	if (this->win && this->win->getXournal()->paste())
	{
		return true;
	}
	return this->clipboardHandler->paste();
}

void Control::selectFillAlpha(bool pen)
{
	XOJ_CHECK_TYPE(Control);

	int alpha = 0;

	if (pen)
	{
		alpha = toolHandler->getPenFill();
	}
	else
	{
		alpha = toolHandler->getHilighterFill();
	}

	FillTransparencyDialog dlg(gladeSearchPath, alpha);
	dlg.show(getGtkWindow());

	if (dlg.getResultAlpha() == -1)
	{
		return;
	}

	alpha = dlg.getResultAlpha();

	if (pen)
	{
		toolHandler->setPenFill(alpha);
	}
	else
	{
		toolHandler->setHilighterFill(alpha);
	}
}

void Control::clearSelectionEndText()
{
	XOJ_CHECK_TYPE(Control);

	clearSelection();
	if (win)
	{
		win->getXournal()->endTextAllPages();
	}
}

/**
 * Fire page selected, but first check if the page Number is valid
 *
 * @return the page ID or size_t_npos if the page is not found
 */
size_t Control::firePageSelected(PageRef page)
{
	XOJ_CHECK_TYPE(Control);

	this->doc->lock();
	size_t pageId = this->doc->indexOf(page);
	this->doc->unlock();
	if (pageId == npos)
	{
		return npos;
	}

	DocumentHandler::firePageSelected(pageId);
	return pageId;
}

void Control::firePageSelected(size_t page)
{
	XOJ_CHECK_TYPE(Control);

	DocumentHandler::firePageSelected(page);
}

void Control::manageToolbars()
{
	XOJ_CHECK_TYPE(Control);

	ToolbarManageDialog dlg(this->gladeSearchPath, this->win->getToolbarModel());
	dlg.show(GTK_WINDOW(this->win->getWindow()));

	this->win->updateToolbarMenu();

	Path file = Util::getConfigFile(TOOLBAR_CONFIG);
	this->win->getToolbarModel()->save(file.str());
}

void Control::customizeToolbars()
{
	XOJ_CHECK_TYPE(Control);

	g_return_if_fail(this->win != nullptr);

	if (this->win->getSelectedToolbar()->isPredefined())
	{
		GtkWidget* dialog = gtk_message_dialog_new(getGtkWindow(),
		                                           GTK_DIALOG_MODAL,
		                                           GTK_MESSAGE_QUESTION,
		                                           GTK_BUTTONS_YES_NO,
		                                           "%s",
		                                           FC(_F("The Toolbarconfiguration \"{1}\" is predefined, "
		                                                 "would you create a copy to edit?") %
		                                              this->win->getSelectedToolbar()->getName()));

		gtk_window_set_transient_for(GTK_WINDOW(dialog), getGtkWindow());
		int res = gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);

		if (res == -8)  // Yes
		{
			ToolbarData* data = new ToolbarData(*this->win->getSelectedToolbar());

			ToolbarModel* model = this->win->getToolbarModel();
			model->initCopyNameId(data);
			model->add(data);
			this->win->toolbarSelected(data);
			this->win->updateToolbarMenu();
		}
		else
		{
			return;
		}
	}

	if (!this->dragDropHandler)
	{
		this->dragDropHandler = new ToolbarDragDropHandler(this);
	}
	this->dragDropHandler->configure();
}

void Control::endDragDropToolbar()
{
	XOJ_CHECK_TYPE(Control);

	if (!this->dragDropHandler)
	{
		return;
	}

	this->dragDropHandler->clearToolbarsFromDragAndDrop();
}

void Control::startDragDropToolbar()
{
	XOJ_CHECK_TYPE(Control);

	if (!this->dragDropHandler)
	{
		return;
	}

	this->dragDropHandler->prepareToolbarsForDragAndDrop();
}

bool Control::isInDragAndDropToolbar()
{
	XOJ_CHECK_TYPE(Control);

	if (!this->dragDropHandler)
	{
		return false;
	}

	return this->dragDropHandler->isInDragAndDrop();
}

void Control::setShapeTool(ActionType type, bool enabled)
{
	XOJ_CHECK_TYPE(Control);

	if (enabled == false)
	{
		// Disable all entries
		this->toolHandler->setDrawingType(DRAWING_TYPE_DEFAULT);

		// fire disabled and return
		fireActionSelected(GROUP_RULER, ACTION_NONE);
		return;
	}

	// Check for nothing changed, and return in this case
	if ((this->toolHandler->getDrawingType() == DRAWING_TYPE_LINE && type == ACTION_RULER) ||
	    (this->toolHandler->getDrawingType() == DRAWING_TYPE_RECTANGLE && type == ACTION_TOOL_DRAW_RECT) ||
	    (this->toolHandler->getDrawingType() == DRAWING_TYPE_ARROW && type == ACTION_TOOL_DRAW_ARROW) ||
	    (this->toolHandler->getDrawingType() == DRAWING_TYPE_COORDINATE_SYSTEM &&
	     type == ACTION_TOOL_DRAW_COORDINATE_SYSTEM) ||
	    (this->toolHandler->getDrawingType() == DRAWING_TYPE_CIRCLE && type == ACTION_TOOL_DRAW_CIRCLE) ||
	    (this->toolHandler->getDrawingType() == DRAWING_TYPE_STROKE_RECOGNIZER && type == ACTION_SHAPE_RECOGNIZER))
	{
		return;
	}

	switch (type)
	{
	case ACTION_TOOL_DRAW_RECT:
		this->toolHandler->setDrawingType(DRAWING_TYPE_RECTANGLE);
		break;

	case ACTION_TOOL_DRAW_CIRCLE:
		this->toolHandler->setDrawingType(DRAWING_TYPE_CIRCLE);
		break;

	case ACTION_TOOL_DRAW_ARROW:
		this->toolHandler->setDrawingType(DRAWING_TYPE_ARROW);
		break;

	case ACTION_TOOL_DRAW_COORDINATE_SYSTEM:
		this->toolHandler->setDrawingType(DRAWING_TYPE_COORDINATE_SYSTEM);
		break;

	case ACTION_RULER:
		this->toolHandler->setDrawingType(DRAWING_TYPE_LINE);
		break;

	case ACTION_SHAPE_RECOGNIZER:
		this->toolHandler->setDrawingType(DRAWING_TYPE_STROKE_RECOGNIZER);
		this->resetShapeRecognizer();
		break;

	default:
		g_warning("Invalid type for setShapeTool: %i", type);
		break;
	}

	fireActionSelected(GROUP_RULER, type);
}

void Control::setFullscreen(bool enabled)
{
	XOJ_CHECK_TYPE(Control);

	fullscreenHandler->setFullscreen(win, enabled);

	fireActionSelected(GROUP_FULLSCREEN, enabled ? ACTION_FULLSCREEN : ACTION_NONE);
}

void Control::disableSidebarTmp(bool disabled)
{
	XOJ_CHECK_TYPE(Control);

	this->sidebar->setTmpDisabled(disabled);
}

void Control::addDefaultPage(string pageTemplate)
{
	XOJ_CHECK_TYPE(Control);

	if (pageTemplate == "")
	{
		pageTemplate = settings->getPageTemplate();
	}

	PageTemplateSettings model;
	model.parse(pageTemplate);

	PageRef page = new XojPage(model.getPageWidth(), model.getPageHeight());
	page->setBackgroundColor(model.getBackgroundColor());
	page->setBackgroundType(model.getBackgroundType());

	this->doc->lock();
	this->doc->addPage(page);
	this->doc->unlock();

	updateDeletePageButton();
}

void Control::updateDeletePageButton()
{
	XOJ_CHECK_TYPE(Control);

	if (this->win)
	{
		GtkWidget* w = this->win->get("menuDeletePage");
		gtk_widget_set_sensitive(w, this->doc->getPageCount() > 1);
	}
}

void Control::deletePage()
{
	XOJ_CHECK_TYPE(Control);

	clearSelectionEndText();
	// don't allow delete pages if we have less than 2 pages,
	// so we can be (more or less) sure there is at least one page.
	if (this->doc->getPageCount() < 2)
	{
		return;
	}

	size_t pNr = getCurrentPageNo();
	if (pNr == npos || pNr > this->doc->getPageCount())
	{
		// something went wrong...
		return;
	}

	this->doc->lock();
	PageRef page = doc->getPage(pNr);
	this->doc->unlock();

	// first send event, then delete page...
	firePageDeleted(pNr);

	this->doc->lock();
	doc->deletePage(pNr);
	this->doc->unlock();

	updateDeletePageButton();
	this->undoRedo->addUndoAction(mem::make_unique<InsertDeletePageUndoAction>(page, pNr, false));

	if (pNr >= this->doc->getPageCount())
	{
		pNr = this->doc->getPageCount() - 1;
	}

	scrollHandler->scrollToPage(pNr, 0);
}

void Control::insertNewPage(size_t position)
{
	XOJ_CHECK_TYPE(Control);

	pageBackgroundChangeController->insertNewPage(position);
}

void Control::insertPage(const PageRef& page, size_t position)
{
	XOJ_CHECK_TYPE(Control);

	this->doc->lock();
	this->doc->insertPage(page, position);
	this->doc->unlock();
	firePageInserted(position);

	getCursor()->updateCursor();

	int visibleHeight = 0;
	scrollHandler->isPageVisible(position, &visibleHeight);

	if (visibleHeight < 10)
	{
		Util::execInUiThread([=]() { scrollHandler->scrollToPage(position); });
	}
	firePageSelected(position);

	updateDeletePageButton();
	undoRedo->addUndoAction(mem::make_unique<InsertDeletePageUndoAction>(page, position, true));
}

void Control::gotoPage()
{
	auto* dlg = new GotoDialog(this->gladeSearchPath, this->doc->getPageCount());

	dlg->show(GTK_WINDOW(this->win->getWindow()));
	int page = dlg->getSelectedPage();

	if (page != -1)
	{
		this->scrollHandler->scrollToPage(page - 1, 0);
	}

	delete dlg;
}

void Control::updateBackgroundSizeButton()
{
	XOJ_CHECK_TYPE(Control);

	if (this->win == nullptr)
	{
		return;
	}

	// Update paper color button
	PageRef p = getCurrentPage();
	if (!p.isValid() || this->win == nullptr)
	{
		return;
	}
	GtkWidget* paperColor = win->get("menuJournalPaperColor");
	GtkWidget* pageSize = win->get("menuJournalPaperFormat");

	PageType bg = p->getBackgroundType();
	gtk_widget_set_sensitive(paperColor, !bg.isSpecial());

	// PDF page size is defined, you cannot change it
	gtk_widget_set_sensitive(pageSize, !bg.isPdfPage());
}

void Control::paperTemplate()
{
	XOJ_CHECK_TYPE(Control);

	auto* dlg = new PageTemplateDialog(this->gladeSearchPath, settings, pageTypes);
	dlg->show(GTK_WINDOW(this->win->getWindow()));

	if (dlg->isSaved())
	{
		newPageType->loadDefaultPage();
	}

	delete dlg;
}

void Control::paperFormat()
{
	XOJ_CHECK_TYPE(Control);

	PageRef page = getCurrentPage();
	if (!page.isValid() || page->getBackgroundType().isPdfPage())
	{
		return;
	}
	clearSelectionEndText();

	auto* dlg = new FormatDialog(this->gladeSearchPath, settings, page->getWidth(), page->getHeight());
	dlg->show(GTK_WINDOW(this->win->getWindow()));

	double width = dlg->getWidth();
	double height = dlg->getHeight();

	if (width > 0)
	{
		this->doc->lock();
		this->doc->setPageSize(page, width, height);
		this->doc->unlock();
	}

	size_t pageNo = doc->indexOf(page);
	if (pageNo != npos && pageNo < doc->getPageCount())
	{
		this->firePageSizeChanged(pageNo);
	}

	delete dlg;
}

void Control::changePageBackgroundColor()
{
	XOJ_CHECK_TYPE(Control);

	int pNr = getCurrentPageNo();
	this->doc->lock();
	PageRef p = this->doc->getPage(pNr);
	this->doc->unlock();

	if (!p.isValid())
	{
		return;
	}

	clearSelectionEndText();

	PageType bg = p->getBackgroundType();
	if (bg.isSpecial())
	{
		return;
	}

	SelectBackgroundColorDialog dlg(this);
	dlg.show(GTK_WINDOW(this->win->getWindow()));
	int color = dlg.getSelectedColor();

	if (color != -1)
	{
		p->setBackgroundColor(color);
		firePageChanged(pNr);
	}
}

void Control::setViewPairedPages(bool enabled)
{
	XOJ_CHECK_TYPE(Control);

	settings->setShowPairedPages(enabled);
	fireActionSelected(GROUP_PAIRED_PAGES, enabled ? ACTION_VIEW_PAIRED_PAGES : ACTION_NOT_SELECTED);

	int currentPage = getCurrentPageNo();
	win->getXournal()->layoutPages();
	scrollHandler->scrollToPage(currentPage);
}

void Control::setViewPresentationMode(bool enabled)
{
	XOJ_CHECK_TYPE(Control);

	if (enabled)
	{
		bool success = zoom->updateZoomPresentationValue();
		if (!success)
		{
			g_warning("Error calculating zoom value");
			fireActionSelected(GROUP_PRESENTATION_MODE, ACTION_NOT_SELECTED);
			return;
		}
	}
	else
	{
		if (settings->isViewFixedRows())
		{
			setViewRows(settings->getViewRows());
		}
		else
		{
			setViewColumns(settings->getViewColumns());
		}

		setViewLayoutVert(settings->getViewLayoutVert());
		setViewLayoutR2L(settings->getViewLayoutR2L());
		setViewLayoutB2T(settings->getViewLayoutB2T());
	}
	zoom->setZoomPresentationMode(enabled);
	settings->setPresentationMode(enabled);

	// Disable Zoom
	fireEnableAction(ACTION_ZOOM_IN, !enabled);
	fireEnableAction(ACTION_ZOOM_OUT, !enabled);
	fireEnableAction(ACTION_ZOOM_FIT, !enabled);
	fireEnableAction(ACTION_ZOOM_100, !enabled);
	fireEnableAction(ACTION_FOOTER_ZOOM_SLIDER, !enabled);

	gtk_widget_set_sensitive(win->get("menuitemLayout"), !enabled);
	gtk_widget_set_sensitive(win->get("menuitemViewDimensions"), !enabled);

	// disable selection of scroll hand tool
	fireEnableAction(ACTION_TOOL_HAND, !enabled);
	fireActionSelected(GROUP_PRESENTATION_MODE, enabled ? ACTION_VIEW_PRESENTATION_MODE : ACTION_NOT_SELECTED);

	int currentPage = getCurrentPageNo();
	win->getXournal()->layoutPages();
	scrollHandler->scrollToPage(currentPage);
}

void Control::setPairsOffset(int numOffset)
{
	XOJ_CHECK_TYPE(Control);

	settings->setPairsOffset(numOffset);
	fireActionSelected(GROUP_PAIRED_PAGES, numOffset ? ACTION_SET_PAIRS_OFFSET : ACTION_NOT_SELECTED);
	int currentPage = getCurrentPageNo();
	win->getXournal()->layoutPages();
	scrollHandler->scrollToPage(currentPage);
}

void Control::setViewColumns(int numColumns)
{
	XOJ_CHECK_TYPE(Control);

	settings->setViewColumns(numColumns);
	settings->setViewFixedRows(false);

	ActionType action;

	switch (numColumns)
	{
	case 1:
		action = ACTION_SET_COLUMNS_1;
		break;
	case 2:
		action = ACTION_SET_COLUMNS_2;
		break;
	case 3:
		action = ACTION_SET_COLUMNS_3;
		break;
	case 4:
		action = ACTION_SET_COLUMNS_4;
		break;
	case 5:
		action = ACTION_SET_COLUMNS_5;
		break;
	case 6:
		action = ACTION_SET_COLUMNS_6;
		break;
	case 7:
		action = ACTION_SET_COLUMNS_7;
		break;
	case 8:
		action = ACTION_SET_COLUMNS_8;
		break;
	default:
		action = ACTION_SET_COLUMNS;
	}

	fireActionSelected(GROUP_FIXED_ROW_OR_COLS, action);

	int currentPage = getCurrentPageNo();
	win->getXournal()->layoutPages();
	scrollHandler->scrollToPage(currentPage);
}

void Control::setViewRows(int numRows)
{
	XOJ_CHECK_TYPE(Control);

	settings->setViewRows(numRows);
	settings->setViewFixedRows(true);

	ActionType action;

	switch (numRows)
	{
	case 1:
		action = ACTION_SET_ROWS_1;
		break;
	case 2:
		action = ACTION_SET_ROWS_2;
		break;
	case 3:
		action = ACTION_SET_ROWS_3;
		break;
	case 4:
		action = ACTION_SET_ROWS_4;
		break;
	case 5:
		action = ACTION_SET_ROWS_5;
		break;
	case 6:
		action = ACTION_SET_ROWS_6;
		break;
	case 7:
		action = ACTION_SET_ROWS_7;
		break;
	case 8:
		action = ACTION_SET_ROWS_8;
		break;
	default:
		action = ACTION_SET_ROWS;
	}

	fireActionSelected(GROUP_FIXED_ROW_OR_COLS, action);

	int currentPage = getCurrentPageNo();
	win->getXournal()->layoutPages();
	scrollHandler->scrollToPage(currentPage);
}

void Control::setViewLayoutVert(bool vert)
{
	XOJ_CHECK_TYPE(Control);

	settings->setViewLayoutVert(vert);

	ActionType action;

	if (vert)
	{
		action = ACTION_SET_LAYOUT_VERTICAL;
	}
	else
	{
		action = ACTION_SET_LAYOUT_HORIZONTAL;
	}

	fireActionSelected(GROUP_LAYOUT_HORIZONTAL, action);

	int currentPage = getCurrentPageNo();
	win->getXournal()->layoutPages();
	scrollHandler->scrollToPage(currentPage);
}

void Control::setViewLayoutR2L(bool r2l)
{
	XOJ_CHECK_TYPE(Control);

	settings->setViewLayoutR2L(r2l);

	ActionType action;

	if (r2l)
	{
		action = ACTION_SET_LAYOUT_R2L;
	}
	else
	{
		action = ACTION_SET_LAYOUT_L2R;
	}

	fireActionSelected(GROUP_LAYOUT_LR, action);

	int currentPage = getCurrentPageNo();
	win->getXournal()->layoutPages();
	scrollHandler->scrollToPage(currentPage);
}

void Control::setViewLayoutB2T(bool b2t)
{
	XOJ_CHECK_TYPE(Control);

	settings->setViewLayoutB2T(b2t);

	ActionType action;

	if (b2t)
	{
		action = ACTION_SET_LAYOUT_B2T;
	}
	else
	{
		action = ACTION_SET_LAYOUT_T2B;
	}

	fireActionSelected(GROUP_LAYOUT_TB, action);

	int currentPage = getCurrentPageNo();
	win->getXournal()->layoutPages();
	scrollHandler->scrollToPage(currentPage);
}

/**
 * This callback is used by used to be called later in the UI Thread
 * On slower machine this feels more fluent, therefore this will not
 * be removed
 */
void Control::zoomCallback(ActionType type, bool enabled)
{
	XOJ_CHECK_TYPE(Control);

	switch (type)
	{
	case ACTION_ZOOM_100:
		zoom->zoom100();
		break;
	case ACTION_ZOOM_FIT:
		if (enabled)
		{
			zoom->updateZoomFitValue();
		}
		// enable/disable ZoomFit
		zoom->setZoomFitMode(enabled);
		break;
	case ACTION_ZOOM_IN:
		zoom->zoomOneStep(ZOOM_IN);
		break;
	case ACTION_ZOOM_OUT:
		zoom->zoomOneStep(ZOOM_OUT);
		break;
	default:
		break;
	}
}

size_t Control::getCurrentPageNo()
{
	XOJ_CHECK_TYPE(Control);

	if (this->win)
	{
		return this->win->getXournal()->getCurrentPage();
	}
	return 0;
}

bool Control::searchTextOnPage(string text, int p, int* occures, double* top)
{
	XOJ_CHECK_TYPE(Control);

	return getWindow()->getXournal()->searchTextOnPage(text, p, occures, top);
}

PageRef Control::getCurrentPage()
{
	XOJ_CHECK_TYPE(Control);

	this->doc->lock();
	PageRef p = this->doc->getPage(getCurrentPageNo());
	this->doc->unlock();

	return p;
}

void Control::fileOpened(const char* uri)
{
	XOJ_CHECK_TYPE(Control);

	openFile(uri);
}

void Control::undoRedoChanged()
{
	XOJ_CHECK_TYPE(Control);

	fireEnableAction(ACTION_UNDO, undoRedo->canUndo());
	fireEnableAction(ACTION_REDO, undoRedo->canRedo());

	win->setUndoDescription(undoRedo->undoDescription());
	win->setRedoDescription(undoRedo->redoDescription());

	updateWindowTitle();
}

void Control::undoRedoPageChanged(PageRef page)
{
	XOJ_CHECK_TYPE(Control);

	for (XojPage* p: this->changedPages)
	{
		if (p == (XojPage*) page)
		{
			return;
		}
	}

	XojPage* p = (XojPage*) page;
	this->changedPages.push_back(p);
	p->reference();
}

void Control::selectTool(ToolType type)
{
	XOJ_CHECK_TYPE(Control);

	toolHandler->selectTool(type);

	if (win)
	{
		(win->getXournal()->getViewFor(getCurrentPageNo()))->rerenderPage();
	}
}

void Control::selectDefaultTool()
{
	XOJ_CHECK_TYPE(Control);

	ButtonConfig* cfg = settings->getDefaultButtonConfig();
	cfg->acceptActions(toolHandler);
}

void Control::toolChanged()
{
	XOJ_CHECK_TYPE(Control);

	ToolType type = toolHandler->getToolType();

	// Convert enum values, enums has to be in the same order!
	ActionType at = (ActionType)(type - TOOL_PEN + ACTION_TOOL_PEN);

	fireActionSelected(GROUP_TOOL, at);

	fireEnableAction(ACTION_SELECT_COLOR, toolHandler->hasCapability(TOOL_CAP_COLOR));
	fireEnableAction(ACTION_SELECT_COLOR_CUSTOM, toolHandler->hasCapability(TOOL_CAP_COLOR));

	fireEnableAction(ACTION_RULER, toolHandler->hasCapability(TOOL_CAP_RULER));
	fireEnableAction(ACTION_TOOL_DRAW_RECT, toolHandler->hasCapability(TOOL_CAP_RECTANGLE));
	fireEnableAction(ACTION_TOOL_DRAW_CIRCLE, toolHandler->hasCapability(TOOL_CAP_CIRCLE));
	fireEnableAction(ACTION_TOOL_DRAW_ARROW, toolHandler->hasCapability(TOOL_CAP_ARROW));
	fireEnableAction(ACTION_TOOL_DRAW_COORDINATE_SYSTEM, toolHandler->hasCapability(TOOL_CAP_ARROW));
	fireEnableAction(ACTION_SHAPE_RECOGNIZER, toolHandler->hasCapability(TOOL_CAP_RECOGNIZER));

	bool enableSize = toolHandler->hasCapability(TOOL_CAP_SIZE);

	fireEnableAction(ACTION_SIZE_MEDIUM, enableSize);
	fireEnableAction(ACTION_SIZE_THICK, enableSize);
	fireEnableAction(ACTION_SIZE_FINE, enableSize);
	fireEnableAction(ACTION_SIZE_VERY_THICK, enableSize);
	fireEnableAction(ACTION_SIZE_VERY_THIN, enableSize);

	bool enableFill = toolHandler->hasCapability(TOOL_CAP_FILL);

	fireEnableAction(ACTION_TOOL_FILL, enableFill);


	if (enableSize)
	{
		toolSizeChanged();
	}

	// Update color
	if (toolHandler->hasCapability(TOOL_CAP_COLOR))
	{
		toolColorChanged(false);
	}

	ActionType rulerAction = ACTION_NOT_SELECTED;
	if (toolHandler->getDrawingType() == DRAWING_TYPE_STROKE_RECOGNIZER)
	{
		rulerAction = ACTION_SHAPE_RECOGNIZER;
	}
	else if (toolHandler->getDrawingType() == DRAWING_TYPE_LINE)
	{
		rulerAction = ACTION_RULER;
	}
	else if (toolHandler->getDrawingType() == DRAWING_TYPE_RECTANGLE)
	{
		rulerAction = ACTION_TOOL_DRAW_RECT;
	}
	else if (toolHandler->getDrawingType() == DRAWING_TYPE_CIRCLE)
	{
		rulerAction = ACTION_TOOL_DRAW_CIRCLE;
	}
	else if (toolHandler->getDrawingType() == DRAWING_TYPE_ARROW)
	{
		rulerAction = ACTION_TOOL_DRAW_ARROW;
	}
	else if (toolHandler->getDrawingType() == DRAWING_TYPE_COORDINATE_SYSTEM)
	{
		rulerAction = ACTION_TOOL_DRAW_COORDINATE_SYSTEM;
	}

	fireActionSelected(GROUP_RULER, rulerAction);

	getCursor()->updateCursor();

	if (type != TOOL_TEXT)
	{
		if (win)
		{
			win->getXournal()->endTextAllPages();
		}
	}
}

void Control::eraserSizeChanged()
{
	XOJ_CHECK_TYPE(Control);

	switch (toolHandler->getEraserSize())
	{
	case TOOL_SIZE_FINE:
		fireActionSelected(GROUP_ERASER_SIZE, ACTION_TOOL_ERASER_SIZE_FINE);
		break;
	case TOOL_SIZE_MEDIUM:
		fireActionSelected(GROUP_ERASER_SIZE, ACTION_TOOL_ERASER_SIZE_MEDIUM);
		break;
	case TOOL_SIZE_THICK:
		fireActionSelected(GROUP_ERASER_SIZE, ACTION_TOOL_ERASER_SIZE_THICK);
		break;
	default:
		break;
	}
}

void Control::penSizeChanged()
{
	XOJ_CHECK_TYPE(Control);

	switch (toolHandler->getPenSize())
	{
	case TOOL_SIZE_VERY_FINE:
		fireActionSelected(GROUP_PEN_SIZE, ACTION_TOOL_PEN_SIZE_VERY_THIN);
		break;
	case TOOL_SIZE_FINE:
		fireActionSelected(GROUP_PEN_SIZE, ACTION_TOOL_PEN_SIZE_FINE);
		break;
	case TOOL_SIZE_MEDIUM:
		fireActionSelected(GROUP_PEN_SIZE, ACTION_TOOL_PEN_SIZE_MEDIUM);
		break;
	case TOOL_SIZE_THICK:
		fireActionSelected(GROUP_PEN_SIZE, ACTION_TOOL_PEN_SIZE_THICK);
		break;
	case TOOL_SIZE_VERY_THICK:
		fireActionSelected(GROUP_PEN_SIZE, ACTION_TOOL_PEN_SIZE_VERY_THICK);
		break;
	default:
		break;
	}
}

void Control::hilighterSizeChanged()
{
	XOJ_CHECK_TYPE(Control);

	switch (toolHandler->getHilighterSize())
	{
	case TOOL_SIZE_FINE:
		fireActionSelected(GROUP_HILIGHTER_SIZE, ACTION_TOOL_HILIGHTER_SIZE_FINE);
		break;
	case TOOL_SIZE_MEDIUM:
		fireActionSelected(GROUP_HILIGHTER_SIZE, ACTION_TOOL_HILIGHTER_SIZE_MEDIUM);
		break;
	case TOOL_SIZE_THICK:
		fireActionSelected(GROUP_HILIGHTER_SIZE, ACTION_TOOL_HILIGHTER_SIZE_THICK);
		break;
	default:
		break;
	}
}

void Control::toolSizeChanged()
{
	XOJ_CHECK_TYPE(Control);

	if (toolHandler->getToolType() == TOOL_PEN)
	{
		penSizeChanged();
	}
	else if (toolHandler->getToolType() == TOOL_ERASER)
	{
		eraserSizeChanged();
	}
	else if (toolHandler->getToolType() == TOOL_HILIGHTER)
	{
		hilighterSizeChanged();
	}

	switch (toolHandler->getSize())
	{
	case TOOL_SIZE_NONE:
		fireActionSelected(GROUP_SIZE, ACTION_NONE);
		break;
	case TOOL_SIZE_VERY_FINE:
		fireActionSelected(GROUP_SIZE, ACTION_SIZE_VERY_THICK);
		break;
	case TOOL_SIZE_FINE:
		fireActionSelected(GROUP_SIZE, ACTION_SIZE_FINE);
		break;
	case TOOL_SIZE_MEDIUM:
		fireActionSelected(GROUP_SIZE, ACTION_SIZE_MEDIUM);
		break;
	case TOOL_SIZE_THICK:
		fireActionSelected(GROUP_SIZE, ACTION_SIZE_THICK);
		break;
	case TOOL_SIZE_VERY_THICK:
		fireActionSelected(GROUP_SIZE, ACTION_SIZE_VERY_THIN);
		break;
	}

	getCursor()->updateCursor();
}

void Control::toolFillChanged()
{
	XOJ_CHECK_TYPE(Control);

	fireActionSelected(GROUP_FILL, toolHandler->getFill() != -1 ? ACTION_TOOL_FILL : ACTION_NONE);
	fireActionSelected(GROUP_PEN_FILL, toolHandler->getPenFillEnabled() ? ACTION_TOOL_PEN_FILL : ACTION_NONE);
	fireActionSelected(GROUP_HILIGHTER_FILL,
	                   toolHandler->getHilighterFillEnabled() ? ACTION_TOOL_HILIGHTER_FILL : ACTION_NONE);
}

void Control::toolLineStyleChanged()
{
	XOJ_CHECK_TYPE(Control);

	const LineStyle& lineStyle = toolHandler->getTool(TOOL_PEN).getLineStyle();
	string style = StrokeStyle::formatStyle(lineStyle);

	if (style == "dash")
	{
		fireActionSelected(GROUP_LINE_STYLE, ACTION_TOOL_LINE_STYLE_DASH);
	}
	else if (style == "dashdot")
	{
		fireActionSelected(GROUP_LINE_STYLE, ACTION_TOOL_LINE_STYLE_DASH_DOT);
	}
	else if (style == "dot")
	{
		fireActionSelected(GROUP_LINE_STYLE, ACTION_TOOL_LINE_STYLE_DOT);
	}
	else
	{
		fireActionSelected(GROUP_LINE_STYLE, ACTION_TOOL_LINE_STYLE_PLAIN);
	}
}

/**
 * Select the color for the tool
 *
 * @param userSelection
 * 			true if the user selected the color
 * 			false if the color is selected by a tool change
 * 			and therefore should not be applied to a selection
 */
void Control::toolColorChanged(bool userSelection)
{
	XOJ_CHECK_TYPE(Control);

	fireActionSelected(GROUP_COLOR, ACTION_SELECT_COLOR);
	getCursor()->updateCursor();

	if (userSelection && this->win && toolHandler->getColor() != -1)
	{
		EditSelection* sel = this->win->getXournal()->getSelection();
		if (sel)
		{
			UndoAction* undo = sel->setColor(toolHandler->getColor());
			// move into selection
			undoRedo->addUndoAction(UndoActionPtr(undo));
		}

		TextEditor* edit = getTextEditor();


		if (this->toolHandler->getToolType() == TOOL_TEXT && edit != nullptr)
		{
			// Todo move into selection
			undoRedo->addUndoAction(UndoActionPtr(edit->setColor(toolHandler->getColor())));
		}
	}
}

void Control::setCustomColorSelected()
{
	XOJ_CHECK_TYPE(Control);

	fireActionSelected(GROUP_COLOR, ACTION_SELECT_COLOR_CUSTOM);
}

void Control::showSettings()
{
	XOJ_CHECK_TYPE(Control);

	// take note of some settings before to compare with after
	int selectionColor = settings->getBorderColor();
	bool verticalSpace = settings->getAddVerticalSpace();
	int verticalSpaceAmount = settings->getAddVerticalSpaceAmount();
	bool horizontalSpace = settings->getAddHorizontalSpace();
	int horizontalSpaceAmount = settings->getAddHorizontalSpaceAmount();
	bool bigCursor = settings->isShowBigCursor();
	bool highlightPosition = settings->isHighlightPosition();

	auto* dlg = new SettingsDialog(this->gladeSearchPath, settings, this);
	dlg->show(GTK_WINDOW(this->win->getWindow()));

	// note which settings have changed and act accordingly
	if (selectionColor != settings->getBorderColor())
	{
		win->getXournal()->forceUpdatePagenumbers();
	}

	if (verticalSpace != settings->getAddVerticalSpace() || horizontalSpace != settings->getAddHorizontalSpace() ||
	    verticalSpaceAmount != settings->getAddVerticalSpaceAmount() ||
	    horizontalSpaceAmount != settings->getAddHorizontalSpaceAmount())
	{
		int currentPage = getCurrentPageNo();
		win->getXournal()->layoutPages();
		scrollHandler->scrollToPage(currentPage);
	}

	if (bigCursor != settings->isShowBigCursor() || highlightPosition != settings->isHighlightPosition())
	{
		getCursor()->updateCursor();
	}

	win->updateScrollbarSidebarPosition();

	enableAutosave(settings->isAutosaveEnabled());

	this->zoom->setZoomStep(settings->getZoomStep() / 100.0);
	this->zoom->setZoomStepScroll(settings->getZoomStepScroll() / 100.0);
	this->zoom->setZoom100Value(settings->getDisplayDpi() / 72.0);

	getWindow()->getXournal()->getHandRecognition()->reload();

	TextView::setDpi(settings->getDisplayDpi());

	delete dlg;
}

bool Control::newFile(string pageTemplate)
{
	XOJ_CHECK_TYPE(Control);

	if (!this->close(true))
	{
		return false;
	}

	Document newDoc(this);

	this->doc->lock();
	*doc = newDoc;
	this->doc->unlock();

	addDefaultPage(std::move(pageTemplate));

	fireDocumentChanged(DOCUMENT_CHANGE_COMPLETE);

	fileLoaded();

	return true;
}

/**
 * Check if this is an autosave file, return false in this case and display a user instruction
 */
bool Control::shouldFileOpen(string filename)
{
	// Compare case insensitive, just in case (Windows, FAT Filesystem etc.)

	filename = StringUtils::toLowerCase(filename);
	string basename = StringUtils::toLowerCase(Util::getConfigSubfolder("").str());

	if (basename.size() > filename.size())
	{
		return true;
	}

	filename = filename.substr(0, basename.size());

	if (filename == basename)
	{

		string msg = FS(_F("Do not open Autosave files. They may will be overwritten!\n"
		                   "Copy the files to another folder.\n"
		                   "Files from Folder {1} cannot be opened.") %
		                basename);
		XojMsgBox::showErrorToUser(getGtkWindow(), msg);
		return false;
	}

	return true;
}

bool Control::openFile(Path filename, int scrollToPage, bool forceOpen)
{
	XOJ_CHECK_TYPE(Control);

	if (!forceOpen && !shouldFileOpen(filename.str()))
	{
		return false;
	}

	if (!this->close(false))
	{
		return false;
	}

	if (filename.isEmpty())
	{
		bool attachPdf = false;
		XojOpenDlg dlg(getGtkWindow(), this->settings);
		filename = Path(dlg.showOpenDialog(false, attachPdf).str());

		g_message("%s", (_F("Filename: {1}") % filename.str()).c_str());

		if (filename.isEmpty())
		{
			return false;
		}

		if (!shouldFileOpen(filename.str()))
		{
			return false;
		}
	}

	this->closeDocument();

	// Read template file
	if (filename.hasExtension(".xopt"))
	{
		return loadXoptTemplate(filename);
	}

	if (filename.hasExtension(".pdf"))
	{
		return loadPdf(filename, scrollToPage);
	}

	LoadHandler loadHandler;
	Document* loadedDocument = loadHandler.loadDocument(filename.str());
	if ((loadedDocument != nullptr && loadHandler.isAttachedPdfMissing()) ||
	    !loadHandler.getMissingPdfFilename().empty())
	{
		// give the user a second chance to select a new PDF file, or to discard the PDF


		GtkWidget* dialog = gtk_message_dialog_new(getGtkWindow(),
		                                           GTK_DIALOG_MODAL,
		                                           GTK_MESSAGE_QUESTION,
		                                           GTK_BUTTONS_NONE,
		                                           "%s",
		                                           loadHandler.isAttachedPdfMissing() ?
		                                                   _("The attached background PDF could not be found.") :
		                                                   _("The background PDF could not be found."));

		gtk_dialog_add_button(GTK_DIALOG(dialog), _("Select another PDF"), 1);
		gtk_dialog_add_button(GTK_DIALOG(dialog), _("Remove PDF Background"), 2);
		gtk_dialog_add_button(GTK_DIALOG(dialog), _("Cancel"), 3);
		gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(this->getWindow()->getWindow()));
		int res = gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);

		if (res == 2)  // remove PDF background
		{
			loadHandler.removePdfBackground();
			loadedDocument = loadHandler.loadDocument(filename.str());
		}
		else if (res == 1)  // select another PDF background
		{
			bool attachToDocument = false;
			XojOpenDlg dlg(getGtkWindow(), this->settings);
			Path pdfFilename = Path(dlg.showOpenDialog(true, attachToDocument).str());
			if (!pdfFilename.isEmpty())
			{
				loadHandler.setPdfReplacement(pdfFilename.str(), attachToDocument);
				loadedDocument = loadHandler.loadDocument(filename.str());
			}
		}
	}

	if (!loadedDocument)
	{
		string msg = FS(_F("Error opening file \"{1}\"") % filename.str()) + "\n" + loadHandler.getLastError();
		XojMsgBox::showErrorToUser(getGtkWindow(), msg);

		fileLoaded(scrollToPage);
		return false;
	}
	else
	{
		this->doc->lock();
		this->doc->clearDocument();
		*this->doc = *loadedDocument;
		this->doc->unlock();

		// Set folder as last save path, so the next save will be at the current document location
		// This is important because of the new .xopp format, where Xournal .xoj handled as import,
		// not as file to load
		settings->setLastSavePath(filename.getParentPath());
	}

	fileLoaded(scrollToPage);
	return true;
}

bool Control::loadPdf(const Path& filename, int scrollToPage)
{
	XOJ_CHECK_TYPE(Control);

	LoadHandler loadHandler;

	if (settings->isAutloadPdfXoj())
	{
		Path f = filename;
		f.clearExtensions();
		f += ".xopp";
		Document* tmp = loadHandler.loadDocument(f.str());

		if (tmp == nullptr)
		{
			f = filename;
			f.clearExtensions();
			f += ".xoj";
			tmp = loadHandler.loadDocument(f.str());
		}

		if (tmp)
		{
			this->doc->lock();
			this->doc->clearDocument();
			*this->doc = *tmp;
			this->doc->unlock();

			fileLoaded(scrollToPage);
			return true;
		}
	}

	bool an = annotatePdf(filename, false, false);
	fileLoaded(scrollToPage);
	return an;
}

bool Control::loadXoptTemplate(Path filename)
{
	XOJ_CHECK_TYPE(Control);

	string contents;
	if (!PathUtil::readString(contents, filename))
	{
		return false;
	}
	newFile(contents);
	return true;
}

void Control::fileLoaded(int scrollToPage)
{
	XOJ_CHECK_TYPE(Control);

	this->doc->lock();
	Path file = this->doc->getEvMetadataFilename();
	this->doc->unlock();

	if (!file.isEmpty())
	{
		MetadataEntry md = metadata->getForFile(file.str());
		if (!md.valid)
		{
			md.zoom = -1;
			md.page = 0;
		}

		if (scrollToPage >= 0)
		{
			md.page = scrollToPage;
		}

		loadMetadata(md);
		recent->addRecentFileFilename(file);
	}
	else
	{
		zoom->updateZoomFitValue();
		zoom->setZoomFitMode(true);
	}

	updateWindowTitle();
	win->getXournal()->forceUpdatePagenumbers();
	getCursor()->updateCursor();
	updateDeletePageButton();
}

class MetadataCallbackData
{
public:
	Control* ctrl;
	MetadataEntry md;
};

/**
 * Load the data after processing the document...
 */
bool Control::loadMetadataCallback(MetadataCallbackData* data)
{
	if (!data->md.valid)
	{
		delete data;
		return false;
	}
	ZoomControl* zoom = data->ctrl->zoom;
	if (zoom->isZoomPresentationMode())
	{
		data->ctrl->setViewPresentationMode(true);
	}
	else if (zoom->isZoomFitMode())
	{
		zoom->updateZoomFitValue();
		zoom->setZoomFitMode(true);
	}
	else
	{
		zoom->setZoomFitMode(false);
		zoom->setZoom(data->md.zoom * zoom->getZoom100Value());
	}
	data->ctrl->scrollHandler->scrollToPage(data->md.page);

	delete data;

	// Do not call again!
	return false;
}

void Control::loadMetadata(MetadataEntry md)
{
	auto* data = new MetadataCallbackData();
	data->md = std::move(md);
	data->ctrl = this;

	g_idle_add((GSourceFunc) loadMetadataCallback, data);
}

bool Control::annotatePdf(Path filename, bool attachPdf, bool attachToDocument)
{
	XOJ_CHECK_TYPE(Control);

	if (!this->close(false))
	{
		return false;
	}

	if (filename.isEmpty())
	{
		XojOpenDlg dlg(getGtkWindow(), this->settings);
		filename = Path(dlg.showOpenDialog(true, attachToDocument).str());
		if (filename.isEmpty())
		{
			return false;
		}
	}

	this->closeDocument();

	getCursor()->setCursorBusy(true);

	this->doc->setFilename("");
	bool res = this->doc->readPdf(filename, true, attachToDocument);

	if (res)
	{
		this->recent->addRecentFileFilename(filename.c_str());

		this->doc->lock();
		Path file = this->doc->getEvMetadataFilename();
		this->doc->unlock();
		MetadataEntry md = metadata->getForFile(file.str());
		loadMetadata(md);
	}
	else
	{
		this->doc->lock();
		string errMsg = doc->getLastErrorMsg();
		this->doc->unlock();

		string msg = FS(_F("Error annotate PDF file \"{1}\"\n{2}") % filename.str() % errMsg);
		XojMsgBox::showErrorToUser(getGtkWindow(), msg);
	}
	getCursor()->setCursorBusy(false);

	fireDocumentChanged(DOCUMENT_CHANGE_COMPLETE);

	getCursor()->updateCursor();

	return true;
}

void Control::print()
{
	XOJ_CHECK_TYPE(Control);

	PrintHandler print;
	this->doc->lock();
	print.print(this->doc, getCurrentPageNo());
	this->doc->unlock();
}

void Control::block(string name)
{
	XOJ_CHECK_TYPE(Control);

	if (this->isBlocking)
	{
		return;
	}

	// Disable all gui Control, to get full control over the application
	win->setControlTmpDisabled(true);
	getCursor()->setCursorBusy(true);
	disableSidebarTmp(true);

	this->statusbar = this->win->get("statusbar");
	this->lbState = GTK_LABEL(this->win->get("lbState"));
	this->pgState = GTK_PROGRESS_BAR(this->win->get("pgState"));

	gtk_label_set_text(this->lbState, name.c_str());
	gtk_widget_show(this->statusbar);

	this->maxState = 100;
	this->isBlocking = true;
}

void Control::unblock()
{
	XOJ_CHECK_TYPE(Control);

	if (!this->isBlocking)
	{
		return;
	}

	this->win->setControlTmpDisabled(false);
	getCursor()->setCursorBusy(false);
	disableSidebarTmp(false);

	gtk_widget_hide(this->statusbar);

	this->isBlocking = false;
}

void Control::setMaximumState(int max)
{
	XOJ_CHECK_TYPE(Control);

	this->maxState = max;
}

void Control::setCurrentState(int state)
{
	XOJ_CHECK_TYPE(Control);

	Util::execInUiThread([=]() { gtk_progress_bar_set_fraction(this->pgState, gdouble(state) / this->maxState); });
}

bool Control::save(bool synchron)
{
	XOJ_CHECK_TYPE(Control);

	// clear selection before saving
	clearSelectionEndText();

	this->doc->lock();
	Path filename = this->doc->getFilename();
	this->doc->unlock();

	if (filename.isEmpty())
	{
		if (!showSaveDialog())
		{
			return false;
		}
	}

	auto* job = new SaveJob(this);
	bool result = true;
	if (synchron)
	{
		result = job->save();
		unblock();
		this->resetSavedStatus();
	}
	else
	{
		this->scheduler->addJob(job, JOB_PRIORITY_URGENT);
	}
	job->unref();

	return result;
}

bool Control::showSaveDialog()
{
	XOJ_CHECK_TYPE(Control);

	GtkWidget* dialog = gtk_file_chooser_dialog_new(_("Save File"),
	                                                getGtkWindow(),
	                                                GTK_FILE_CHOOSER_ACTION_SAVE,
	                                                _("_Cancel"),
	                                                GTK_RESPONSE_CANCEL,
	                                                _("_Save"),
	                                                GTK_RESPONSE_OK,
	                                                nullptr);

	gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), true);

	GtkFileFilter* filterXoj = gtk_file_filter_new();
	gtk_file_filter_set_name(filterXoj, _("Xournal++ files"));
	gtk_file_filter_add_pattern(filterXoj, "*.xopp");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filterXoj);

	this->doc->lock();
	Path suggested_folder = this->doc->createSaveFolder(this->settings->getLastSavePath());
	Path suggested_name = this->doc->createSaveFilename(Document::XOPP, this->settings->getDefaultSaveName());
	this->doc->unlock();

	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), suggested_folder.c_str());
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggested_name.c_str());
	gtk_file_chooser_add_shortcut_folder(GTK_FILE_CHOOSER(dialog), this->settings->getLastOpenPath().c_str(), nullptr);

	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), false);  // handled below

	gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(this->getWindow()->getWindow()));

	while (true)
	{
		if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_OK)
		{
			gtk_widget_destroy(dialog);
			return false;
		}

		Path filenameTmp = Path(gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)));
		filenameTmp.clearExtensions();
		filenameTmp += ".xopp";
		Path currentFolder(gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(dialog)));

		// Since we add the extension after the OK button, we have to check manually on existing files
		if (checkExistingFile(currentFolder, filenameTmp))
		{
			break;
		}
	}

	char* name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

	string filename = name;
	char* folder = gtk_file_chooser_get_current_folder_uri(GTK_FILE_CHOOSER(dialog));
	settings->setLastSavePath(folder);
	g_free(folder);
	g_free(name);

	gtk_widget_destroy(dialog);

	this->doc->lock();

	this->doc->setFilename(filename);
	this->doc->unlock();

	return true;
}

void Control::updateWindowTitle()
{
	XOJ_CHECK_TYPE(Control);

	string title = "";

	this->doc->lock();
	if (doc->getFilename().isEmpty())
	{
		if (doc->getPdfFilename().isEmpty())
		{
			title = _("Unsaved Document");
		}
		else
		{
			if (undoRedo->isChanged())
			{
				title += "*";
			}
			title += doc->getPdfFilename().getFilename();
		}
	}
	else
	{
		if (undoRedo->isChanged())
		{
			title += "*";
		}

		title += doc->getFilename().getFilename();
	}
	this->doc->unlock();

	title += " - Xournal++";

	gtk_window_set_title(getGtkWindow(), title.c_str());
}

void Control::exportAsPdf()
{
	XOJ_CHECK_TYPE(Control);

	exportBase(new PdfExportJob(this));
}

void Control::exportAs()
{
	XOJ_CHECK_TYPE(Control);
	exportBase(new CustomExportJob(this));
}

void Control::exportBase(BaseExportJob* job)
{
	if (job->showFilechooser())
	{
		this->scheduler->addJob(job, JOB_PRIORITY_NONE);
	}
	else
	{
		// The job blocked, so we have to unblock, because the job unblocks only after run
		unblock();
	}
	job->unref();
}

bool Control::saveAs()
{
	XOJ_CHECK_TYPE(Control);

	if (!showSaveDialog())
	{
		return false;
	}
	this->doc->lock();
	Path filename = doc->getFilename();
	this->doc->unlock();

	if (filename.isEmpty())
	{
		return false;
	}

	// no lock needed, this is an uncritically operation
	this->doc->setCreateBackupOnSave(false);
	return save();
}

void Control::resetSavedStatus()
{
	this->doc->lock();
	Path filename = this->doc->getFilename();
	this->doc->unlock();

	this->undoRedo->documentSaved();
	this->recent->addRecentFileFilename(filename);
	this->updateWindowTitle();
}

void Control::quit(bool allowCancel)
{
	XOJ_CHECK_TYPE(Control);

	if (!this->close(false, allowCancel))
	{
		if (!allowCancel)
		{
			// Cancel is not allowed, and the user close or did not save
			// This is probably called from macOS, where the Application
			// now will be killed - therefore do an emergency save.
			emergencySave();
		}

		return;
	}

	this->closeDocument();

	this->scheduler->lock();

	audioController->stopRecording();
	settings->save();

	this->scheduler->removeAllJobs();
	this->scheduler->unlock();
	gtk_main_quit();
}

bool Control::close(const bool allowDestroy, const bool allowCancel)
{
	XOJ_CHECK_TYPE(Control);

	clearSelectionEndText();
	metadata->documentChanged();

	bool discard = false;
	const bool fileRemoved = !doc->getFilename().isEmpty() && !this->doc->getFilename().exists();
	if (undoRedo->isChanged())
	{
		const auto message = fileRemoved ? _("Document file was removed.") : _("This document is not saved yet.");
		const auto saveLabel = fileRemoved ? _("Save As...") : _("Save");
		GtkWidget* dialog = gtk_message_dialog_new(getGtkWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
		                                           GTK_BUTTONS_NONE, "%s", message);

		gtk_dialog_add_button(GTK_DIALOG(dialog), saveLabel, GTK_RESPONSE_ACCEPT);
		gtk_dialog_add_button(GTK_DIALOG(dialog), _("Discard"), GTK_RESPONSE_REJECT);

		if (allowCancel)
		{
			gtk_dialog_add_button(GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
		}

		gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(this->getWindow()->getWindow()));
		const auto dialogResponse = gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);

		switch (dialogResponse)
		{
		case GTK_RESPONSE_ACCEPT:
			if (fileRemoved)
			{
				return this->saveAs();
			}
			else
			{
				return this->save(true);
			}
			break;
		case GTK_RESPONSE_REJECT:
			discard = true;
			break;
		default:
			return false;
			break;
		}
	}

	if (allowDestroy && discard)
	{
		this->closeDocument();
	}
	return true;
}

bool Control::closeAndDestroy(bool allowCancel)
{
	// We don't want to "double close", so disallow it first.
	auto retval = this->close(false, allowCancel);
	this->closeDocument();
	return retval;
}

void Control::closeDocument()
{
	this->undoRedo->clearContents();

	this->doc->lock();
	this->doc->clearDocument(true);
	this->doc->unlock();

	this->undoRedoChanged();
}

bool Control::checkExistingFile(Path& folder, Path& filename)
{
	XOJ_CHECK_TYPE(Control);

	if (filename.exists())
	{
		string msg = FS(FORMAT_STR("The file {1} already exists! Do you want to replace it?") % filename.getFilename());
		int res = XojMsgBox::replaceFileQuestion(getGtkWindow(), msg);
		return res != 1;  // res != 1 when user clicks on Replace
	}
	return true;
}

void Control::resetShapeRecognizer()
{
	XOJ_CHECK_TYPE(Control);

	if (this->win)
	{
		this->win->getXournal()->resetShapeRecognizer();
	}
}

void Control::showAbout()
{
	XOJ_CHECK_TYPE(Control);

	AboutDialog dlg(this->gladeSearchPath);
	dlg.show(GTK_WINDOW(this->win->getWindow()));
}

void Control::clipboardCutCopyEnabled(bool enabled)
{
	XOJ_CHECK_TYPE(Control);

	fireEnableAction(ACTION_CUT, enabled);
	fireEnableAction(ACTION_COPY, enabled);
}

void Control::clipboardPasteEnabled(bool enabled)
{
	XOJ_CHECK_TYPE(Control);

	fireEnableAction(ACTION_PASTE, enabled);
}

void Control::clipboardPasteText(string text)
{
	XOJ_CHECK_TYPE(Control);

	Text* t = new Text();
	t->setText(text);
	t->setFont(settings->getFont());
	t->setColor(toolHandler->getColor());

	clipboardPaste(t);
}

void Control::clipboardPasteImage(GdkPixbuf* img)
{
	XOJ_CHECK_TYPE(Control);

	auto image = new Image();
	image->setImage(img);

	auto width = static_cast<double>(gdk_pixbuf_get_width(img)) / settings->getDisplayDpi() * 72.0;
	auto height = static_cast<double>(gdk_pixbuf_get_height(img)) / settings->getDisplayDpi() * 72.0;

	int pageNr = getCurrentPageNo();
	if (pageNr == -1)
	{
		return;
	}

	this->doc->lock();
	PageRef page = this->doc->getPage(pageNr);
	auto pageWidth = page->getWidth();
	auto pageHeight = page->getHeight();
	this->doc->unlock();

	// Size: 3/4 of the page size
	pageWidth = pageWidth * 3.0 / 4.0;
	pageHeight = pageHeight * 3.0 / 4.0;

	auto scaledWidth = width;
	auto scaledHeight = height;

	if (width > pageWidth)
	{
		scaledWidth = pageWidth;
		scaledHeight = (scaledWidth * height) / width;
	}

	if (scaledHeight > pageHeight)
	{
		scaledHeight = pageHeight;
		scaledWidth = (scaledHeight * width) / height;
	}

	image->setWidth(scaledWidth);
	image->setHeight(scaledHeight);

	clipboardPaste(image);
}

void Control::clipboardPaste(Element* e)
{
	XOJ_CHECK_TYPE(Control);

	double x = 0;
	double y = 0;
	int pageNr = getCurrentPageNo();
	if (pageNr == -1)
	{
		return;
	}

	XojPageView* view = win->getXournal()->getViewFor(pageNr);
	if (view == nullptr)
	{
		return;
	}

	this->doc->lock();
	PageRef page = this->doc->getPage(pageNr);
	Layer* layer = page->getSelectedLayer();
	win->getXournal()->getPasteTarget(x, y);

	double width = e->getElementWidth();
	double height = e->getElementHeight();

	x = MAX(0, x - width / 2);
	y = MAX(0, y - height / 2);

	e->setX(x);
	e->setY(y);
	layer->addElement(e);

	this->doc->unlock();

	undoRedo->addUndoAction(mem::make_unique<InsertUndoAction>(page, layer, e));
	EditSelection* selection = new EditSelection(this->undoRedo, e, view, page);

	win->getXournal()->setSelection(selection);
}

void Control::clipboardPasteXournal(ObjectInputStream& in)
{
	XOJ_CHECK_TYPE(Control);

	int pNr = getCurrentPageNo();
	if (pNr == -1 && win != nullptr)
	{
		return;
	}

	this->doc->lock();
	PageRef page = this->doc->getPage(pNr);
	Layer* layer = page->getSelectedLayer();

	XojPageView* view = win->getXournal()->getViewFor(pNr);

	if (!view || !page)
	{
		this->doc->unlock();
		return;
	}

	EditSelection* selection = nullptr;
	try
	{
		std::unique_ptr<Element> element;
		string version = in.readString();
		if (version != PROJECT_STRING)
		{
			g_warning("Paste from Xournal Version %s to Xournal Version %s", version.c_str(), PROJECT_STRING);
		}

		selection = new EditSelection(this->undoRedo, page, view);
		selection->readSerialized(in);

		// document lock not needed anymore, because we don't change the document, we only change the selection
		this->doc->unlock();

		int count = in.readInt();
		auto pasteAddUndoAction = mem::make_unique<AddUndoAction>(page, false);
		// this will undo a group of elements that are inserted

		for (int i = 0; i < count; i++)
		{
			string name = in.getNextObjectName();
			element.reset();

			if (name == "Stroke")
			{
				element = mem::make_unique<Stroke>();
			}
			else if (name == "Image")
			{
				element = mem::make_unique<Image>();
			}
			else if (name == "TexImage")
			{
				element = mem::make_unique<TexImage>();
			}
			else if (name == "Text")
			{
				element = mem::make_unique<Text>();
			}
			else
			{
				throw InputStreamException(FS(FORMAT_STR("Get unknown object {1}") % name), __FILE__, __LINE__);
			}

			element->readSerialized(in);

			pasteAddUndoAction->addElement(layer, element.get(), layer->indexOf(element.get()));
			// Todo: unique_ptr
			selection->addElement(element.release());
		}
		undoRedo->addUndoAction(std::move(pasteAddUndoAction));

		win->getXournal()->setSelection(selection);
	}
	catch (std::exception& e)
	{
		g_warning("could not paste, Exception occurred: %s", e.what());
		Stacktrace::printStracktrace();
		if (selection)
		{
			for (Element* e: *selection->getElements())
			{
				delete e;
			}
			delete selection;
		}
	}
}

void Control::deleteSelection()
{
	XOJ_CHECK_TYPE(Control);

	if (win)
	{
		win->getXournal()->deleteSelection();
	}
}

void Control::clearSelection()
{
	XOJ_CHECK_TYPE(Control);

	if (this->win)
	{
		this->win->getXournal()->clearSelection();
	}
}

void Control::setClipboardHandlerSelection(EditSelection* selection)
{
	XOJ_CHECK_TYPE(Control);

	if (this->clipboardHandler)
	{
		this->clipboardHandler->setSelection(selection);
	}
}

void Control::setCopyPasteEnabled(bool enabled)
{
	XOJ_CHECK_TYPE(Control);

	this->clipboardHandler->setCopyPasteEnabled(enabled);
}

void Control::setFill(bool fill)
{
	XOJ_CHECK_TYPE(Control);

	EditSelection* sel = nullptr;
	if (this->win)
	{
		sel = this->win->getXournal()->getSelection();
	}

	if (sel)
	{
		undoRedo->addUndoAction(UndoActionPtr(
		        sel->setFill(fill ? toolHandler->getPenFill() : -1, fill ? toolHandler->getHilighterFill() : -1)));
	}

	if (toolHandler->getToolType() == TOOL_PEN)
	{
		fireActionSelected(GROUP_PEN_FILL, fill ? ACTION_TOOL_PEN_FILL : ACTION_NONE);
		this->toolHandler->setPenFillEnabled(fill, false);
	}
	else if (toolHandler->getToolType() == TOOL_HILIGHTER)
	{
		fireActionSelected(GROUP_HILIGHTER_FILL, fill ? ACTION_TOOL_HILIGHTER_FILL : ACTION_NONE);
		this->toolHandler->setHilighterFillEnabled(fill, false);
	}
}

void Control::setLineStyle(const string& style)
{
	XOJ_CHECK_TYPE(Control);

	LineStyle stl = StrokeStyle::parseStyle(style.c_str());

	EditSelection* sel = nullptr;
	if (this->win)
	{
		sel = this->win->getXournal()->getSelection();
	}

	// TODO allow to change selection
	if (sel)
	{
		//		UndoAction* undo = sel->setSize(size, toolHandler->getToolThickness(TOOL_PEN),
		//										toolHandler->getToolThickness(TOOL_HILIGHTER),
		//										toolHandler->getToolThickness(TOOL_ERASER));
		//		undoRedo->addUndoAction(undo);
	}

	this->toolHandler->setLineStyle(stl);
}

void Control::setToolSize(ToolSize size)
{
	XOJ_CHECK_TYPE(Control);

	EditSelection* sel = nullptr;
	if (this->win)
	{
		sel = this->win->getXournal()->getSelection();
	}

	if (sel)
	{
		undoRedo->addUndoAction(UndoActionPtr(sel->setSize(size,
		                                                   toolHandler->getToolThickness(TOOL_PEN),
		                                                   toolHandler->getToolThickness(TOOL_HILIGHTER),
		                                                   toolHandler->getToolThickness(TOOL_ERASER))));
	}
	this->toolHandler->setSize(size);
}

void Control::fontChanged()
{
	XOJ_CHECK_TYPE(Control);

	XojFont font = win->getFontButtonFont();
	settings->setFont(font);

	EditSelection* sel = nullptr;
	if (this->win)
	{
		sel = this->win->getXournal()->getSelection();
	}
	if (sel)
	{
		undoRedo->addUndoAction(UndoActionPtr(sel->setFont(font)));
	}

	TextEditor* editor = getTextEditor();
	if (editor)
	{
		editor->setFont(font);
	}
}

/**
 * The core handler for inserting latex
 */
void Control::runLatex()
{
	XOJ_CHECK_TYPE(Control);

	LatexController latex(this);
	latex.run();
}

/**
 * GETTER / SETTER
 */

UndoRedoHandler* Control::getUndoRedoHandler()
{
	XOJ_CHECK_TYPE(Control);

	return this->undoRedo;
}

ZoomControl* Control::getZoomControl()
{
	XOJ_CHECK_TYPE(Control);

	return this->zoom;
}

XournalppCursor* Control::getCursor()
{
	XOJ_CHECK_TYPE(Control);

	return this->cursor;
}

RecentManager* Control::getRecentManager()
{
	XOJ_CHECK_TYPE(Control);

	return this->recent;
}

Document* Control::getDocument()
{
	XOJ_CHECK_TYPE(Control);

	return this->doc;
}

ToolHandler* Control::getToolHandler()
{
	XOJ_CHECK_TYPE(Control);

	return this->toolHandler;
}

XournalScheduler* Control::getScheduler()
{
	XOJ_CHECK_TYPE(Control);

	return this->scheduler;
}

MainWindow* Control::getWindow()
{
	XOJ_CHECK_TYPE(Control);

	return this->win;
}

GtkWindow* Control::getGtkWindow()
{
	XOJ_CHECK_TYPE(Control);

	return GTK_WINDOW(this->win->getWindow());
}

bool Control::isFullscreen()
{
	XOJ_CHECK_TYPE(Control);
	return this->fullscreenHandler->isFullscreen();
}

void Control::rotationSnappingToggle()
{
	XOJ_CHECK_TYPE(Control);

	settings->setSnapRotation(!settings->isSnapRotation());
	fireActionSelected(GROUP_SNAPPING, settings->isSnapRotation() ? ACTION_ROTATION_SNAPPING : ACTION_NONE);
}

void Control::gridSnappingToggle()
{
	XOJ_CHECK_TYPE(Control);

	settings->setSnapGrid(!settings->isSnapGrid());
	fireActionSelected(GROUP_GRID_SNAPPING, settings->isSnapGrid() ? ACTION_GRID_SNAPPING : ACTION_NONE);
}

TextEditor* Control::getTextEditor()
{
	XOJ_CHECK_TYPE(Control);

	if (this->win)
	{
		return this->win->getXournal()->getTextEditor();
	}
	return nullptr;
}

GladeSearchpath* Control::getGladeSearchPath()
{
	XOJ_CHECK_TYPE(Control);

	return this->gladeSearchPath;
}

Settings* Control::getSettings()
{
	XOJ_CHECK_TYPE(Control);

	return settings;
}

ScrollHandler* Control::getScrollHandler()
{
	XOJ_CHECK_TYPE(Control);

	return this->scrollHandler;
}

MetadataManager* Control::getMetadataManager()
{
	XOJ_CHECK_TYPE(Control);

	return this->metadata;
}

Sidebar* Control::getSidebar()
{
	XOJ_CHECK_TYPE(Control);

	return this->sidebar;
}

SearchBar* Control::getSearchBar()
{
	XOJ_CHECK_TYPE(Control);

	return this->searchBar;
}

AudioController* Control::getAudioController()
{
	XOJ_CHECK_TYPE(Control);

	return this->audioController;
}

PageTypeHandler* Control::getPageTypes()
{
	XOJ_CHECK_TYPE(Control);

	return this->pageTypes;
}

PageTypeMenu* Control::getNewPageType()
{
	XOJ_CHECK_TYPE(Control);

	return this->newPageType;
}

PageBackgroundChangeController* Control::getPageBackgroundChangeController()
{
	XOJ_CHECK_TYPE(Control);

	return this->pageBackgroundChangeController;
}

LayerController* Control::getLayerController()
{
	XOJ_CHECK_TYPE(Control);

	return this->layerController;
}

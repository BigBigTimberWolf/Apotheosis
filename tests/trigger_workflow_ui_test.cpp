#include "widgets/TriggerWorkflowEditor.h"
#include "widgets/TriggerTargetEditor.h"
#include "config.h"
#include <QApplication>
#include <QGraphicsView>
#include <QGraphicsProxyWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QDir>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QFontDatabase>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    app.setFont(QFont("Microsoft YaHei"));
    TriggerWorkflowEditor editor;
    editor.resize(850, 400);
    HotkeyProfile in;
    in.trigger_enabled = true;
    in.trigger_auto_scope = 2;
    in.trigger_auto_stop = 1;
    in.trigger_weapon_switch31 = true;
    in.trigger_fire_delay = 135;
    in.trigger_prearm_enabled = true;
    in.trigger_prearm_expand_percent = 80;
    in.trigger_loss_delay_ms = 170;
    editor.load(in);
    HotkeyProfile out;
    editor.save(out);
    CHECK(out.trigger_enabled && out.trigger_auto_scope == 2 && out.trigger_auto_stop == 1);
    CHECK(out.trigger_weapon_switch31 && out.trigger_fire_delay == 135);
    auto* fireMode = editor.findChild<QComboBox*>("triggerFireMode");
    CHECK(fireMode && fireMode->count() == 4 && out.trigger_fire_mode == 3);
    for (int i = 0; i < 4; ++i) {
        fireMode->setCurrentIndex(i);
        TriggerParams saved;
        editor.save(saved);
        CHECK(saved.trigger_fire_mode == i);
    }

    CHECK(!out.trigger_prearm_enabled && out.trigger_prearm_expand_percent == 80);
    auto* prearmExpand = editor.findChild<QSpinBox*>("triggerPrearmExpand");
    CHECK(prearmExpand && !prearmExpand->isEnabled());
    auto* lossDelay = editor.findChild<QSpinBox*>("triggerLossDelay");
    CHECK(lossDelay && !lossDelay->isEnabled() && out.trigger_loss_delay_ms == 170);
    TriggerParams secondary;
    secondary.trigger_enabled = true;
    secondary.trigger_fire_delay = 260;
    secondary.trigger_auto_stop = 2;
    secondary.trigger_loss_delay_ms = 230;
    editor.load(secondary);
    TriggerParams savedSecondary;
    editor.save(savedSecondary);
    CHECK(savedSecondary.trigger_enabled && savedSecondary.trigger_fire_delay == 260 &&
          savedSecondary.trigger_auto_stop == 2);
    CHECK(savedSecondary.trigger_loss_delay_ms == 230 && lossDelay->isEnabled());
    secondary.trigger_fire_duration = 20;
    editor.load(secondary);
    CHECK(!lossDelay->isEnabled());
    editor.load(in);
    int nodes = 0;
    QPushButton* switchNode = nullptr;
    QPushButton* fireNode = nullptr;
    auto* canvas = editor.findChild<QGraphicsView*>();
    CHECK(canvas && canvas->scene());
    for (auto* item : canvas->scene()->items()) {
        auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(item);
        auto* button = proxy ? qobject_cast<QPushButton*>(proxy->widget()) : nullptr;
        if (!button) continue;
        if (!button->property("nodeNumber").isValid()) continue;
        ++nodes;
        if (button->property("nodeNumber").toString() == QStringLiteral("07")) switchNode = button;
        if (button->property("nodeNumber").toString() == QStringLiteral("05")) fireNode = button;
        CHECK(!button->text().contains(QString::fromUtf8(u8"瞬狙")));
    }
    CHECK(nodes == 7 && switchNode && fireNode);
    QPushButton* returnBranch = nullptr;
    QPushButton* spinBranch = nullptr;
    QPushButton* normalBranch = nullptr;
    int branchNodes = 0;
    for (auto* item : canvas->scene()->items()) {
        auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(item);
        auto* button = proxy ? qobject_cast<QPushButton*>(proxy->widget()) : nullptr;
        if (!button || !button->property("branchMode").isValid()) continue;
        ++branchNodes;
        if (button->property("branchMode").toInt() == 0) normalBranch = button;
        if (button->property("branchStep").toInt() == 0) {
            if (button->property("branchMode").toInt() == 1) returnBranch = button;
            if (button->property("branchMode").toInt() == 2) spinBranch = button;
        }
    }
    CHECK(branchNodes == 13 && normalBranch && returnBranch && spinBranch &&
          !returnBranch->toolTip().isEmpty() && !spinBranch->toolTip().isEmpty());
    switchNode->click();
    auto* inspector = editor.findChild<QStackedWidget*>();
    CHECK(inspector && inspector->currentIndex() == 6);
    editor.show();
    app.processEvents();
    auto* zoomIn = editor.findChild<QPushButton*>("workflowZoomIn");
    auto* fit = editor.findChild<QPushButton*>("workflowFitButton");
    CHECK(zoomIn && fit && canvas->width() > 500);
    fit->click();
    const qreal fitted = canvas->transform().m11();
    zoomIn->click();
    CHECK(canvas->transform().m11() > fitted);
    fit->click();
    CHECK(qAbs(canvas->transform().m11() - fitted) < 0.01);
    editor.resize(600, editor.height());
    app.processEvents();
    CHECK(canvas->transform().m11() < fitted);
    CHECK(editor.grab().save(QDir::current().filePath("trigger_workflow_preview.png")));
    in.trigger_weapon_switch31 = false;
    editor.load(in);
    fireNode->click();
    editor.resize(850, 500);
    app.processEvents();
    CHECK(editor.grab().save(QDir::current().filePath("trigger_loss_delay_preview.png")));

    in.trigger_mode = 2;
    in.trigger_spin_turns = 3;
    in.trigger_spin_counts_per_turn = 14000;
    in.trigger_spin_step_degrees = 90;
    in.trigger_spin_step_ms = 40;
    in.trigger_spin_hold_ms = 350;
    in.trigger_snap_fire_hold_ms = 45;
    in.trigger_snap_cooldown_ms = 280;
    in.trigger_flash_disappear_ms = 140;
    editor.load(in);
    editor.resize(1200, 900);
    auto* mode = editor.findChild<QComboBox*>("triggerMode");
    CHECK(mode && mode->currentData().toInt() == 2 && !mode->isVisible() &&
          canvas->isVisible() && spinBranch->isChecked());
    app.processEvents();
    CHECK(editor.grab().save(QDir::current().filePath("trigger_snap_mode_preview.png")));
    HotkeyProfile snapSaved;
    editor.save(snapSaved);
    CHECK(snapSaved.trigger_mode == 2 && snapSaved.trigger_spin_turns == 3 &&
          snapSaved.trigger_snap_fire_hold_ms == 45 &&
          snapSaved.trigger_snap_cooldown_ms == 280 &&
          snapSaved.trigger_flash_disappear_ms == 140 &&
          snapSaved.trigger_fire_duration == in.trigger_fire_duration);
    CHECK(!editor.findChild<QSpinBox*>("snapSharedFireDuration"));
    auto* disappear = editor.findChild<QSpinBox*>("triggerFlashDisappearMs");
    auto* turnCounts = editor.findChild<QSpinBox*>("triggerSpinCountsPerTurn");
    auto* stepDegrees = editor.findChild<QSpinBox*>("triggerTurnStepDegrees");
    auto* stepMs = editor.findChild<QSpinBox*>("triggerTurnDurationMs");
    auto* holdMs = editor.findChild<QSpinBox*>("triggerTurnHoldMs");
    CHECK(disappear && disappear->value() == 140 && turnCounts &&
          stepDegrees && stepMs && holdMs && turnCounts->value() == 14000 &&
          turnCounts->maximum() == 200000 &&
          stepDegrees->value() == 90 && stepMs->value() == 40 &&
          holdMs->value() == 350 && holdMs->maximum() == 10000);
    CHECK(!editor.findChild<QPushButton*>("triggerCalibrationMove") &&
          !editor.findChild<QSpinBox*>("triggerSpinSpeed"));
    disappear->setValue(40);
    turnCounts->setValue(120000);
    stepDegrees->setValue(120);
    stepMs->setValue(25);
    holdMs->setValue(750);
    editor.save(snapSaved);
    CHECK(snapSaved.trigger_flash_disappear_ms == 40 &&
          snapSaved.trigger_spin_counts_per_turn == 120000 &&
          snapSaved.trigger_spin_step_degrees == 120 &&
          snapSaved.trigger_spin_step_ms == 25 &&
          snapSaved.trigger_spin_hold_ms == 750 &&
          snapSaved.trigger_fire_duration == in.trigger_fire_duration);
    CHECK(!spinBranch->text().contains(QString::fromUtf8(u8"单次开火")));
    returnBranch->click();
    CHECK(mode->currentData().toInt() == 1 && returnBranch->isChecked() &&
          !spinBranch->isChecked() && canvas->isVisible());
    CHECK(!inspector->isVisible());
    auto* returnY = editor.findChild<QSpinBox*>("triggerReturnYPercent");
    CHECK(returnY && returnY->value() == 75);
    returnY->setValue(60);
    app.processEvents();
    CHECK(editor.grab().save(QDir::current().filePath("trigger_return_mode_preview.png")));
    fireNode->click();
    CHECK(inspector->isVisible());
    TriggerParams returnSaved;
    editor.save(returnSaved);
    CHECK(returnSaved.trigger_mode == 1 && returnSaved.trigger_spin_turns == 3 &&
          returnSaved.trigger_return_y_percent == 60);
    normalBranch->click();
    CHECK(mode->currentData().toInt() == 0 && canvas->isVisible() &&
          inspector->isVisible());
    TriggerParams classicSaved;
    editor.save(classicSaved);
    CHECK(classicSaved.trigger_mode == 0);

    TriggerTargetEditor targets;
    HotkeyProfile targetProfile;
    targetProfile.trigger_classes = {{2, 0.25f, 0.8f, 30, 45}};
    targets.load(targetProfile, {{2, "head", ClassBucket::Aim}});
    auto* width = targets.findChild<QSpinBox*>("triggerRangeX");
    auto* height = targets.findChild<QSpinBox*>("triggerRangeY");
    auto* pointY = targets.findChild<QDoubleSpinBox*>("triggerPointY");
    CHECK(width && height && pointY && targets.findChild<QWidget*>("triggerZonePreview"));
    CHECK(width->maximum() == 1000 && height->maximum() == 1000);
    width->setValue(1000);
    height->setValue(1000);
    pointY->setValue(0.7);
    HotkeyProfile savedTarget;
    targets.save(savedTarget);
    CHECK(savedTarget.trigger_classes.size() == 1 &&
          savedTarget.trigger_classes[0].class_id == 2 &&
          savedTarget.trigger_classes[0].range_x_percent == 1000 &&
          savedTarget.trigger_classes[0].range_y_percent == 1000 &&
          qAbs(savedTarget.trigger_classes[0].y_offset - 0.7f) < 0.001f);
}

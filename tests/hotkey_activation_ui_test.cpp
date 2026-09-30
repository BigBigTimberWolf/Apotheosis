#include "widgets/HotkeyActivationWidget.h"
#include "keyboard/hotkey_selection.h"
#include <QApplication>
#include <QComboBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPushButton>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
int main(int argc, char** argv)
{
    QApplication app(argc,argv);
#ifdef _WIN32
    const int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(), 10));
#endif
    HotkeyActivationWidget widget;
    std::vector<HotkeyProfile> profiles(2);
    profiles[0].name = "Rifle"; profiles[1].name = "Sniper";
    profiles[0].keys = {"RightMouseButton"};
    profiles[1].keys = {"X2MouseButton"};
    widget.load(profiles,0);
    CHECK(widget.isHidden());
    profiles[1].keys = profiles[0].keys;
    widget.load(profiles,0);
    auto* button = widget.findChild<QPushButton*>("activateHotkeyButton");
    auto* key = widget.findChild<QComboBox*>("hotkeyActivationKey");
    auto* status = widget.findChild<QLabel*>("hotkeyActivationStatus");
    CHECK(!widget.isHidden() && button && key && status && button->isEnabled());
    QObject::connect(&widget, &HotkeyActivationWidget::activateRequested, &widget, [&] {
        activateHotkey(profiles,0); widget.load(profiles,0);
    });
    button->click();
    CHECK(preferredHotkey(profiles,0) == 0 && !button->isEnabled());
    int changes = 0;
    QObject::connect(&widget, &HotkeyActivationWidget::activationKeyChanged, &widget, [&](const QString& value) {
        ++changes; profiles[0].activation_key = value.toStdString();
    });
    key->setCurrentIndex(key->findData("Key1"));
    CHECK(changes == 1 && profiles[0].activation_key == "Key1");
    widget.load(profiles,0);
    CHECK(changes == 1);
    profiles[1].activation_key = "Key1";
    widget.load(profiles,0);
    CHECK(status->text().contains(QStringLiteral("不可用")));
    profiles[1].group = "Other";
    widget.load(profiles,0);
    CHECK(widget.isHidden());
    profiles[1].group = profiles[0].group;
    profiles[1].activation_key = "Key2";
    widget.load(profiles,0);
    widget.resize(540,240); widget.show(); app.processEvents();
    CHECK(widget.grab().save("hotkey_activation_preview.png"));
}

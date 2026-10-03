#include "Apotheosis.h"
#include "pages/AutoCapturePage.h"
#include "widgets/ToggleSwitch.h"
#include <QApplication>
#include <QLabel>
#include <cstdio>
namespace AutoCapture { void reset_session_counter() {} }
int main(int argc,char** argv) {
    QApplication app(argc,argv);int failures=0;
    auto check=[&](bool okay,const char* message){if(!okay){std::printf("FAIL: %s\n",message);++failures;}};
    config.auto_capture_enabled=true;
    config.auto_capture_trigger_only=true;
    AutoCapturePage page;
    auto* only=page.findChild<ToggleSwitch*>("autoCaptureTriggerOnly");
    auto* hint=page.findChild<QLabel*>("autoCaptureConfigHint");
    check(only&&only->isChecked(),"trigger-only setting loads into the real capture page");
    check(hint&&hint->text().contains(QStringLiteral("成功发送开火")),"hint describes confirmed automatic fire");
    if(only) {
        only->setChecked(false);check(!config.auto_capture_trigger_only,"turning the option off saves normal capture mode");
        only->setChecked(true);check(config.auto_capture_trigger_only,"turning the option on saves trigger-only mode");
    }
    return failures?1:0;
}

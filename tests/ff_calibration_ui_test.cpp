#include "widgets/FfCalibrationDialog.h"
#include "ff_calibration_simulation.h"
#include <QApplication>
#include <cstdio>

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    int failures=0;
    auto check=[&](bool okay,const char* message) {if(!okay){std::printf("FAIL: %s\n",message);++failures;}};
    int appliedBank=-1;control::FfCalibrationResult applied;
    FfCalibrationDialog dialog(3,QStringLiteral("测试热键"),{},
        [&](int bank,const control::FfCalibrationResult& r){appliedBank=bank;applied=r;return true;});
    auto* bank=dialog.findChild<QComboBox*>("ffCalibrationBank");
    auto* confirmed=dialog.findChild<QCheckBox*>("ffCalibrationStaticTarget");
    auto* start=dialog.findChild<QPushButton*>("ffCalibrationStart");
    auto* apply=dialog.findChild<QPushButton*>("ffCalibrationApply");
    auto* timer=dialog.findChild<QTimer*>();
    check(bank && confirmed && start && apply && timer,"calibration controls exist");
    if(!bank||!confirmed||!start||!apply||!timer) return 1;
    check(!start->isEnabled() && !apply->isEnabled(),"no accidental start or unvalidated apply");
    bank->setCurrentIndex(1);confirmed->setChecked(true);start->click();
    auto& session=runtime::FfCalibrationSession::instance();
    check(session.snapshot().bank==1 && !bank->isEnabled(),"chosen bank is captured for the session");
    simulateFfCalibration(3);
    QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection);
    check(!apply->isEnabled() && appliedBank==-1,"finished samples do not change settings while the hotkey is held");
    session.released();QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection);
    check(apply->isEnabled() && appliedBank==-1,"valid result waits for explicit apply");
    apply->click();
    check(appliedBank==1 && applied.valid && std::abs(applied.pixelsPerCount.x-1.35)<.02,
          "explicit apply targets the recorded bank with measured gain");
    {
        FfCalibrationDialog reset(3,QStringLiteral("设备重置"),{},
            [&](int,const auto&){++appliedBank;return true;});
        reset.findChild<QCheckBox*>("ffCalibrationStaticTarget")->setChecked(true);
        reset.findChild<QPushButton*>("ffCalibrationStart")->click();
        simulateFfCalibration(3);session.released();
        QMetaObject::invokeMethod(reset.findChild<QTimer*>(),"timeout",Qt::DirectConnection);
        auto* resetApply=reset.findChild<QPushButton*>("ffCalibrationApply");
        check(resetApply->isEnabled(),"finished result is initially available");
        session.cancel("设备已重置");resetApply->click();
        check(appliedBank==1 && !resetApply->isEnabled(),"device reset invalidates a cached calibration result");
    }
    {
        FfCalibrationDialog cancelled(3,QStringLiteral("取消"),{},[](int,const auto&){return false;});
        cancelled.findChild<QCheckBox*>("ffCalibrationStaticTarget")->setChecked(true);
        cancelled.findChild<QPushButton*>("ffCalibrationStart")->click();
        check(session.active(),"second session started");cancelled.reject();
        check(!session.active(),"closing the dialog cancels probes");
    }
    // 标定启动键：按一下等于点一次“开始标定”，之后仍然按住瞄准热键才开始采样。
    {
        session.cancel();
        FfCalibrationDialog unconfirmed(3,QStringLiteral("未确认"),{},[](int,const auto&){return true;},
            nullptr,QStringLiteral("LeftMouseButton"));
        auto* unconfirmedTimer=unconfirmed.findChild<QTimer*>();
        auto tap=[&] {
            runtime::g_ffCalibrationKeyTaps.fetch_add(1);
            QMetaObject::invokeMethod(unconfirmedTimer,"timeout",Qt::DirectConnection);
        };
        tap();
        check(!session.active(),"start key without the static-target confirmation cannot arm");
        unconfirmed.findChild<QCheckBox*>("ffCalibrationStaticTarget")->setChecked(true);
        tap();
        const auto armed=session.snapshot();
        check(session.active() && armed.state==runtime::FfCalibrationSession::State::Armed,
            "tapping the start key arms the session exactly like the button");
        unconfirmed.reject();
        check(!session.active(),"closing the dialog cancels a key-armed session");
    }
    runtime::g_ffCalibrationKeyTaps.store(0);
    return failures?1:0;
}

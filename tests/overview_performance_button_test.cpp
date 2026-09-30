#include "pages/OverviewPage.h"

#include <QApplication>
#include <QPushButton>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    OverviewPage page;
    auto* button = page.findChild<QPushButton*>("performanceModeButton");
    CHECK(button && button->isEnabled());
    bool requested = false;
    int calls = 0;
    QObject::connect(&page, &OverviewPage::performanceModeRequested,
                     &page, [&](bool enabled) { requested = enabled; ++calls; });

    page.setPerformanceMode(false);
    CHECK(button->isEnabled());
    button->click();
    CHECK(calls == 1 && requested);

    page.setPerformanceMode(true);
    CHECK(button->isEnabled());
    button->click();
    CHECK(calls == 2 && !requested);
}

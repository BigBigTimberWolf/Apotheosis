#pragma once

#include <QStringList>
#include <QWidget>

class QButtonGroup;
class QVBoxLayout;
class QLabel;

class SideNav : public QWidget {
    Q_OBJECT

public:
    explicit SideNav(QWidget* parent = nullptr);

    void setItems(const QString& groupTitle, const QStringList& labels,
                  const QStringList& iconNames);
    void setCurrentIndex(int index);
    int currentIndex() const;

signals:
    void currentChanged(int index);

private:
    void recolorIcons();

    QLabel* m_title{};
    QVBoxLayout* m_itemsLayout{};
    QButtonGroup* m_group{};
    QStringList m_icons;
};

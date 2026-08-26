#pragma once

#include <QFont>
#include <QString>
#include <QStringList>

namespace aida::qt::theme::fonts {

struct mono_grid_t {
    qreal cell_w = 0.0;
    qreal cell_h = 0.0;
    qreal line_h = 0.0;
    bool fixed_pitch = false;
    bool uniform_advance = false;
    bool valid = false;
};

bool load();
bool loaded();

QString uiFamily();
QString codeFamily();
QStringList uiFamilies();
QStringList codeFamilies();

QFont ui(int weight, int pixelSize);
QFont code(int weight, int pixelSize);

QFont body();
QFont bodyEm();
QFont strong();
QFont h1();
QFont h2();
QFont large();
QFont caption();
QFont display();
QFont codeRegular();
QFont codeEm();
QFont codeLarge();

const mono_grid_t& monoGrid();
void invalidateMonoGrid();

}

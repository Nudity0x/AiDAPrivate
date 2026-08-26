#pragma once

#include <QString>

namespace aida::qt::clipboard {

void set_text(const QString& text);
QString text();
bool has_text();

}

#pragma once

#include <QHash>
#include <QString>

class QWidget;

namespace aida::qt::theme {

struct tokens_t;

namespace stylesheet {

QString liveDirectory();
QString loadTemplate();

QHash<QString, QString> tokenSubstitutions(const tokens_t& t);
QString substituteTokens(const QString& templateText, const tokens_t& t);
QString renderStylesheet(const tokens_t& t);

void applyToApplication(const tokens_t& t);
void repolish(QWidget* w);
void clearCaches();

}

}

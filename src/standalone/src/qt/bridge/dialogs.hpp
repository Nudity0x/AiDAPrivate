#pragma once

#include <QString>
#include <QStringList>

#include <optional>
#include <string>
#include <vector>

class QWidget;

namespace aida::qt::dialogs {

QString filter_from_win32_pairs(const char* filter_pairs);

std::optional<std::string> open_file(QWidget* parent, const QString& title,
                                     const char* filter_pairs,
                                     const QString& start_dir = QString());
std::vector<std::string> open_files(QWidget* parent, const QString& title,
                                    const char* filter_pairs,
                                    const QString& start_dir = QString());
std::optional<std::string> save_file(QWidget* parent, const QString& title,
                                     const char* filter_pairs,
                                     const QString& default_extension = QString(),
                                     const QString& start_dir = QString());
std::optional<std::string> choose_directory(QWidget* parent, const QString& title,
                                            const QString& start_dir = QString());

}

#include "qt/bridge/dialogs.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>

#include <cstring>

#include "helpers/diag_log.hpp"

namespace aida::qt::dialogs {

namespace {

constexpr std::size_t k_max_filter_scan = 4096;

struct filter_pair_t {
    QString name;
    QString patterns;
};

bool parse_filter_pairs(const char* filter_pairs, std::vector<filter_pair_t>& out) {
    if (!filter_pairs)
        return false;
    std::size_t index = 0;
    bool last_was_zero = false;
    bool terminated = false;
    while (index < k_max_filter_scan) {
        if (filter_pairs[index] == '\0') {
            if (last_was_zero) {
                terminated = true;
                break;
            }
            last_was_zero = true;
        } else {
            last_was_zero = false;
        }
        ++index;
    }
    if (!terminated)
        return false;

    const char* cursor = filter_pairs;
    while (*cursor != '\0') {
        const std::size_t name_length = std::strlen(cursor);
        if (name_length == 0)
            return false;
        QString name = QString::fromUtf8(cursor, static_cast<qsizetype>(name_length));
        cursor += name_length + 1;
        if (*cursor == '\0')
            return false;
        const std::size_t pattern_length = std::strlen(cursor);
        if (pattern_length == 0)
            return false;
        QString patterns = QString::fromUtf8(cursor, static_cast<qsizetype>(pattern_length));
        cursor += pattern_length + 1;
        out.push_back({std::move(name), std::move(patterns)});
    }
    return !out.empty();
}

QString normalize_patterns(const QString& patterns) {
    QString normalized = patterns;
    normalized.replace(u';', u' ');
    normalized = normalized.simplified();
    return normalized;
}

QString convert_pair(const filter_pair_t& pair) {
    const QString patterns = normalize_patterns(pair.patterns);
    QString name = pair.name.trimmed();
    if (name.endsWith(u')')) {
        const qsizetype open = name.lastIndexOf(u'(');
        if (open >= 0) {
            const QString inner = normalize_patterns(
                name.mid(open + 1, name.size() - open - 2));
            if (inner == patterns)
                name = name.left(open).trimmed();
        }
    }
    return name + QStringLiteral(" (") + patterns + u')';
}

std::string to_utf8(const QString& value) {
    const QByteArray encoded = value.toUtf8();
    return std::string(encoded.constData(), static_cast<std::size_t>(encoded.size()));
}

}

QString filter_from_win32_pairs(const char* filter_pairs) {
    std::vector<filter_pair_t> pairs;
    if (!parse_filter_pairs(filter_pairs, pairs)) {
        if (filter_pairs) {
            diag::log_tagged_fmt("qt_dialogs",
                "filter_pairs_invalid caller=qt reason=malformed_or_missing_double_nul");
        }
        return QStringLiteral("All files (*.*)");
    }
    QStringList converted;
    converted.reserve(static_cast<qsizetype>(pairs.size()));
    for (const auto& pair : pairs)
        converted.push_back(convert_pair(pair));
    return converted.join(QStringLiteral(";;"));
}

std::optional<std::string> open_file(QWidget* parent, const QString& title,
                                     const char* filter_pairs,
                                     const QString& start_dir) {
    const QString filter = filter_from_win32_pairs(filter_pairs);
    const QString selected = QFileDialog::getOpenFileName(parent, title, start_dir, filter);
    if (selected.isEmpty())
        return std::nullopt;
    return to_utf8(QDir::toNativeSeparators(selected));
}

std::vector<std::string> open_files(QWidget* parent, const QString& title,
                                    const char* filter_pairs,
                                    const QString& start_dir) {
    const QString filter = filter_from_win32_pairs(filter_pairs);
    const QStringList selected =
        QFileDialog::getOpenFileNames(parent, title, start_dir, filter);
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(selected.size()));
    for (const QString& path : selected)
        result.push_back(to_utf8(QDir::toNativeSeparators(path)));
    return result;
}

std::optional<std::string> save_file(QWidget* parent, const QString& title,
                                     const char* filter_pairs,
                                     const QString& default_extension,
                                     const QString& start_dir) {
    const QString filter = filter_from_win32_pairs(filter_pairs);
    QString selected = QFileDialog::getSaveFileName(parent, title, start_dir, filter);
    if (selected.isEmpty())
        return std::nullopt;
    QString extension = default_extension;
    if (extension.startsWith(u'.'))
        extension.remove(0, 1);
    if (!extension.isEmpty() && QFileInfo(selected).suffix().isEmpty())
        selected += u'.' + extension;
    return to_utf8(QDir::toNativeSeparators(selected));
}

std::optional<std::string> choose_directory(QWidget* parent, const QString& title,
                                            const QString& start_dir) {
    const QString selected = QFileDialog::getExistingDirectory(parent, title, start_dir);
    if (selected.isEmpty())
        return std::nullopt;
    return to_utf8(QDir::toNativeSeparators(selected));
}

}

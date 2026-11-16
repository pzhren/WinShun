#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

namespace ws {

// Most-recently-searched queries, newest first. Shown for an empty query when
// the start list is 最近搜索; Enter on such a row puts the query back in the
// box and searches it. One line per query, the query alone (a newline in a
// query becomes a space when it is recorded).
//
// Not part of History: the paths opened are not the words looked for, and
// these are cleared on their own (自动清除最近搜索, N seconds after the
// window hides).
class SearchHistory {
public:
    explicit SearchHistory(QString filePath, int capacity = 20);

    void load();
    void record(const QString& query);
    void remove(const QString& query);
    void clear();
    const QStringList& items() const noexcept { return m_items; }

private:
    void save() const;
    static QString key(const QString& query) { return query.toCaseFolded(); }

    QString m_filePath;
    int m_capacity;
    QStringList m_items;
    QSet<QString> m_seen; // by key()
};

} // namespace ws

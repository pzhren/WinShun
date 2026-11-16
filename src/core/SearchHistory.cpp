#include "SearchHistory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace ws {

SearchHistory::SearchHistory(QString filePath, int capacity)
    : m_filePath(std::move(filePath))
    , m_capacity(capacity)
{
}

void SearchHistory::load()
{
    m_items.clear();
    m_seen.clear();
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    while (!file.atEnd() && m_items.size() < m_capacity) {
        const QString query = QString::fromUtf8(file.readLine()).trimmed();
        if (query.isEmpty() || m_seen.contains(key(query)))
            continue;
        m_items.append(query);
        m_seen.insert(key(query));
    }
}

void SearchHistory::record(const QString& query)
{
    QString q = query.simplified(); // newlines and runs of space out; a query holds one line
    if (q.isEmpty() || q.size() > 200)
        return;
    if (m_items.value(0).compare(q, Qt::CaseInsensitive) == 0)
        return; // the same words again: nothing moves
    if (m_items.removeIf([&](const QString& s) { return s.compare(q, Qt::CaseInsensitive) == 0; }) == 0)
        m_seen.insert(key(q));
    m_items.prepend(q);
    while (m_items.size() > m_capacity)
        m_seen.remove(key(m_items.takeLast()));
    save();
}

void SearchHistory::remove(const QString& query)
{
    if (m_items.removeIf([&](const QString& s) { return s.compare(query, Qt::CaseInsensitive) == 0; }) > 0) {
        m_seen.remove(key(query));
        save();
    }
}

void SearchHistory::clear()
{
    if (m_items.isEmpty())
        return;
    m_items.clear();
    m_seen.clear();
    save();
}

void SearchHistory::save() const
{
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    for (const QString& query : m_items) {
        file.write(query.toUtf8());
        file.write("\n");
    }
    file.commit();
}

} // namespace ws

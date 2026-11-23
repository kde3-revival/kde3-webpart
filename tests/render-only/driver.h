/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPE_RENDER_ONLY_DRIVER_H
#define KWPE_RENDER_ONLY_DRIVER_H

#include <qobject.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qvaluelist.h>
#include <kurl.h>

namespace KParts { class ReadOnlyPart; struct URLArgs; }

// One reader (a part in mail mode, as KMail creates it) showing a message
// in phases: one showing per phase, render only ("r") or with external
// references ("e"), AFTER ms apart; the message's host names carry the
// reader and phase (*.a1.blocked.test, *.a2.blocked.test, ...).  Reports
// on stdout, each line starting with the reader's letter: PHASE <n> <r|e>,
// WIDGET <x> <y>, COMPLETED, LINK <url> (what a click handed to the host).
class Reader : public QObject
{
    Q_OBJECT
public:
    Reader( char tag, KParts::ReadOnlyPart* part, const QStringList& modes, const QStringList& dirs, int afterMs );
    KParts::ReadOnlyPart* part() const { return m_part; }

public slots:
    void slotCompleted();
    void slotLink( const KURL& url, const KParts::URLArgs& );
    void slotNextPhase();

private:
    char m_tag;
    KParts::ReadOnlyPart* m_part;
    QStringList m_modes;
    QStringList m_dirs;
    int m_afterMs;
    unsigned m_phase;
};

// The end: a screenshot of the first reader, RENDERED <red pixels> (the
// messages carry a red marker box: layout and paint happened).
class Finisher : public QObject
{
    Q_OBJECT
public:
    Finisher( KParts::ReadOnlyPart* part, int waitMs, const QString& shot );

public slots:
    void slotFinish();

private:
    KParts::ReadOnlyPart* m_part;
    QString m_shot;
};

#endif

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
#include <kurl.h>

namespace KParts { class ReadOnlyPart; }
namespace KParts { struct URLArgs; }

// Loads the part in mail mode (as KMail does), shows one message and
// reports on stdout: WIDGET <x> <y>, COMPLETED, LINK <url> (what a click
// handed to the host), RENDERED <red pixels> (layout and paint happened: the messages
// carry a red marker box).
class Driver : public QObject
{
    Q_OBJECT
public:
    Driver( KParts::ReadOnlyPart* part, int waitMs, const QString& shot );
    void again( int ms, const KURL& url );

public slots:
    void slotCompleted();
    void slotLink( const KURL& url, const KParts::URLArgs& );
    void slotAgain();
    void slotFinish();

private:
    KParts::ReadOnlyPart* m_part;
    int m_waitMs;
    QString m_shot;
    KURL m_again;
};

#endif

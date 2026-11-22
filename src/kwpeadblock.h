/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPEADBLOCK_H
#define KWPEADBLOCK_H

#include <qdatetime.h>
#include <qobject.h>
#include <qstringlist.h>
#include <qvaluelist.h>

#include <kio/global.h>

// WebKit's type, without GLib (Qt 3's "signals" macro, see kwpewidget.h)
typedef struct _WebKitUserContentManager WebKitUserContentManager;

class KProcess;
class KURL;
namespace KIO { class Job; }

/**
 * The ad blocker: Adblock Plus filter lists (EasyList, EasyPrivacy, ...)
 * as WebKit content filters, compiled once into WebKit's byte code and
 * applied by WebKit itself to every load (no per-request call into the
 * part).
 *
 * The lists are downloaded over HTTPS with KIO (no cookies, the
 * certificate must validate, size-limited) into
 * $KDEHOME/share/apps/kwpepart/contentfilters/lists, converted and
 * compiled by kwpe-filterconv in the background into the filter store
 * there, then attached to every view.  Weekly, or with updateNow().  A
 * new set of filters gets new identifiers: until it is complete, the
 * old one stays in use (and in kwpepartrc); a half-done update leaves
 * nothing behind that is used.
 *
 * Sites where blocking is off: the views showing them have no filters
 * (WebKit's "ignore-previous-rules" works only inside one rule list).
 */
class KWPEAdBlock : public QObject
{
    Q_OBJECT

public:
    struct List
    {
        QString id;       /// "easylist", ..., "custom-<hash>"
        QString name;
        QString url;      /// https only
        bool enabled;
        bool custom;
    };

    static KWPEAdBlock* self();

    /** A view and its content manager: gets the filters when they are ready. */
    void addView( WebKitUserContentManager* manager );
    void removeView( WebKitUserContentManager* manager );
    /**
     * The view is about to show a page of @p host: filters on or off for
     * it.  True if that switched them (on to off or off to on).
     */
    bool setViewHost( WebKitUserContentManager* manager, const QString& host );
    /**
     * A frame of the view is about to show a page of @p host (a subframe,
     * or the main frame before setViewHost()): its scriptlets, if any,
     * are injected into it.
     */
    void addFrameHost( WebKitUserContentManager* manager, const QString& host );

    bool isEnabled() const;
    void setEnabled( bool enabled );
    /** uBlock's scriptlets ("site##+js(...)") run in the pages. */
    bool scriptletsEnabled() const;
    void setScriptletsEnabled( bool enabled );
    /** Hours between two updates of the lists. */
    int updateInterval() const;
    void setUpdateInterval( int hours );

    /** The site list contains @p host or a parent domain of it. */
    bool isSiteDisabled( const QString& host ) const;
    /** Blocking off (on) for @p host (and its subdomains). */
    void setSiteDisabled( const QString& host, bool disabled );
    QStringList disabledSites() const;
    void setDisabledSites( const QStringList& sites );

    QValueList<List> lists() const;
    /** The lists to use (built-in ones with their flag, custom ones). */
    void setLists( const QValueList<List>& lists );

    /** Download and compile now (in the background). */
    void updateNow();
    bool isUpdating() const;
    QDateTime lastUpdate() const;
    /** One line about the state: rules, last update, what failed, progress. */
    QString status() const;
    /** Filters are attached to views showing @p host. */
    bool isBlocking( const QString& host ) const;
    /**
     * Not reading filters from the store (at the start of a process):
     * a page loaded now is filtered, if filtering is on.
     */
    bool isReady() const;

    /** Read kwpepartrc again (another process changed it), apply it. */
    void reloadConfig();

signals:
    /** status(), the filters or the site list changed. */
    void changed();

private slots:
    void checkSchedule();
    void slotData( KIO::Job* job, const QByteArray& data );
    void slotRedirection( KIO::Job* job, const KURL& url );
    void slotTotalSize( KIO::Job* job, KIO::filesize_t size );
    void slotResult( KIO::Job* job );
    void slotConverterOutput( KProcess* process, char* buffer, int length );
    void slotConverterExited( KProcess* process );

private:
    KWPEAdBlock();
    friend struct KWPEAdBlockGlue;
    void readConfig();
    void loadFilters( const QStringList& identifiers, bool fromUpdate );
    void filtersLoaded( unsigned serial );
    void applyToViews();
    bool apply( WebKitUserContentManager* manager );
    void applyScripts( WebKitUserContentManager* manager );
    void loadScriptlets( const QStringList& identifiers );
    QString scriptletsSource( const QString& host );
    void downloadNext();
    void convert();
    void finishUpdate( bool ok, const QString& message );
    void setStatus( const QString& status );
};

#endif

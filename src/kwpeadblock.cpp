/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// WebKit (GLib) before any Qt header: Qt 3's "signals"/"slots" macros.
#include <wpe/webkit.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qmap.h>
#include <qregexp.h>
#include <qtimer.h>

#include <kconfig.h>
#include <kglobal.h>
#include <kio/job.h>
#include <klocale.h>
#include <klockfile.h>
#include <kmdcodec.h>
#include <kprocess.h>
#include <ksavefile.h>
#include <kstandarddirs.h>
#include <kurl.h>

#include "kwpeadblock.h"
#include "kwpesettings.h"

namespace
{

struct BuiltinList
{
    const char* id;
    const char* name;
    const char* url;
    bool enabled;
};

const BuiltinList builtinLists[] = {
    { "easylist", I18N_NOOP( "EasyList" ), "https://easylist.to/easylist/easylist.txt", true },
    { "easyprivacy", I18N_NOOP( "EasyPrivacy" ), "https://easylist.to/easylist/easyprivacy.txt", true },
    { "ublock", I18N_NOOP( "uBlock filters" ), "https://ublockorigin.github.io/uAssets/filters/filters.txt", false },
    { "ublock-annoyances", I18N_NOOP( "uBlock filters \xe2\x80\x93 Annoyances" ),
      "https://ublockorigin.github.io/uAssets/filters/annoyances.txt", false },
    { "rolist", I18N_NOOP( "ROList (Romanian)" ), "https://www.zoso.ro/pages/rolist.txt", false },
};
const unsigned builtinCount = sizeof( builtinLists ) / sizeof( builtinLists[0] );

const unsigned maxListSize = 16 * 1024 * 1024;   // EasyList: 2 MiB
const unsigned maxIncludes = 32;                 // uBlock filters: 10
const int updateIntervalDays = 7;
const int retryMinutes = 60;                     // after a failed update
const char filterPrefix[] = "kwpe";

struct Download
{
    QString listId;
    QString listName;
    KURL url;
    QString path;      // where it goes
    bool include;      // a "!#include" of the list
};

struct State
{
    State() : enabled( true ), rules( 0 ), store( 0 ), setSerial( 0 ), loadSerial( 0 ), loadStart( 0 ), loadPending( 0 ),
              loadFailed( false ), loadFromUpdate( false ), updating( false ), job( 0 ), total( 0 ), includes( 0 ), process( 0 ) {}

    bool enabled;
    QValueList<KWPEAdBlock::List> lists;
    QStringList sites;
    QStringList filterIds;      // in kwpepartrc: the set to use
    QDateTime lastUpdate;
    QDateTime lastAttempt;
    QString lastResult;
    QString status;
    unsigned rules;

    WebKitUserContentFilterStore* store;
    QValueList<WebKitUserContentFilter*> filters;   // the set attached to views (refs)
    QStringList filtersIds;                          // ... their identifiers
    unsigned setSerial;                              // changes with the set
    QMap<WebKitUserContentManager*, QString> viewHost;
    QMap<WebKitUserContentManager*, unsigned> viewSet;   // set attached (0: none)

    // loading a set from the store
    unsigned loadSerial;
    gint64 loadStart;
    QStringList loadIds;
    QValueList<WebKitUserContentFilter*> loading;
    unsigned loadPending;
    bool loadFailed;
    bool loadFromUpdate;

    // updating
    bool updating;
    KLockFile::Ptr lock;
    QValueList<Download> queue;
    KIO::Job* job;
    Download current;
    QByteArray data;
    KIO::filesize_t total;      // announced size of the download (0: unknown)
    unsigned includes;
    QStringList files;          // the lists for the converter
    QStringList errors;
    KProcess* process;
    QCString output;
    QString newPrefix;
    QString newHash;
    QStringList newIds;
    unsigned newRules;
    QString newStats;
};

State s;
KWPEAdBlock* s_self = 0;

QString baseDir()
{
    return KGlobal::dirs()->saveLocation( "data", "kwpepart/contentfilters/" );
}

QString storeDir()
{
    return baseDir() + "store";
}

QString listDir( const QString& id )
{
    return KGlobal::dirs()->saveLocation( "data", "kwpepart/contentfilters/lists/" + id + "/" );
}

QString normalizedHost( const QString& host )
{
    QString h = host.lower().stripWhiteSpace();
    if ( h.endsWith( "." ) )
        h.truncate( h.length() - 1 );
    return h;
}

bool hostMatches( const QString& host, const QString& site )
{
    return host == site || host.endsWith( "." + site );
}

QString customId( const QString& url )
{
    KMD5 md5( url.utf8() );
    return "custom-" + QString::fromLatin1( md5.hexDigest().left( 12 ) );
}

// A time in kwpepartrc (seconds since 1970; invalid if missing or 0).
QDateTime readTime( KConfig* c, const char* key )
{
    QDateTime t;
    unsigned seconds = c->readUnsignedNumEntry( key, 0 );
    if ( seconds )
        t.setTime_t( seconds );
    return t;
}

// Lists come over HTTPS, or from a local file (a list of one's own).
bool isAllowedURL( const KURL& url )
{
    if ( !url.isValid() || !url.user().isEmpty() || !url.pass().isEmpty() )
        return false;
    if ( url.protocol() == "https" )
        return !url.host().isEmpty();
    return url.isLocalFile() && url.host().isEmpty() && url.path().startsWith( "/" );
}

}

// GLib callbacks (friend of KWPEAdBlock)
struct KWPEAdBlockGlue
{
    static void loaded( GObject* store, GAsyncResult* result, gpointer data )
    {
        unsigned serial = GPOINTER_TO_UINT( data );
        GError* error = 0;
        WebKitUserContentFilter* filter =
            webkit_user_content_filter_store_load_finish( WEBKIT_USER_CONTENT_FILTER_STORE( store ), result, &error );
        if ( serial != s.loadSerial ) {
            // superseded by a newer load
            if ( filter )
                webkit_user_content_filter_unref( filter );
            if ( error )
                g_error_free( error );
            return;
        }
        if ( filter ) {
            s.loading.append( filter );
        } else {
            s.loadFailed = true;
            qWarning( "kwpepart: content filter not loaded: %s", error ? error->message : "?" );
        }
        if ( error )
            g_error_free( error );
        if ( --s.loadPending == 0 )
            s_self->filtersLoaded( serial );
    }

    // After an update: the identifiers in the store, every one of ours
    // that is not in use goes.  Only now the lock is given back: another
    // process may start compiling a new set right after.
    static void identifiers( GObject* store, GAsyncResult* result, gpointer )
    {
        gchar** ids = webkit_user_content_filter_store_fetch_identifiers_finish( WEBKIT_USER_CONTENT_FILTER_STORE( store ), result );
        for ( gchar** id = ids; id && *id; ++id ) {
            QString name = QString::fromUtf8( *id );
            if ( name.startsWith( filterPrefix ) && !s.filterIds.contains( name ) )
                webkit_user_content_filter_store_remove( s.store, *id, 0, removed, 0 );
        }
        g_strfreev( ids );
        // downloaded lists nobody uses any more
        QDir lists( baseDir() + "lists" );
        QStringList dirs = lists.entryList( QDir::Dirs | QDir::NoSymLinks );
        for ( QStringList::ConstIterator it = dirs.begin(); it != dirs.end(); ++it ) {
            bool used = false;
            for ( QValueList<KWPEAdBlock::List>::ConstIterator l = s.lists.begin(); l != s.lists.end(); ++l )
                if ( ( *l ).id == *it && ( *l ).enabled )
                    used = true;
            if ( used || *it == "." || *it == ".." )
                continue;
            QDir dir( lists.filePath( *it ) );
            QStringList files = dir.entryList( QDir::Files | QDir::Hidden | QDir::System );
            for ( QStringList::ConstIterator f = files.begin(); f != files.end(); ++f )
                dir.remove( *f );
            lists.rmdir( *it );
        }
        s_self->finishUpdate( true, QString::null );
    }

    static void removed( GObject* store, GAsyncResult* result, gpointer )
    {
        GError* error = 0;
        if ( !webkit_user_content_filter_store_remove_finish( WEBKIT_USER_CONTENT_FILTER_STORE( store ), result, &error ) ) {
            qWarning( "kwpepart: old content filter not removed: %s", error ? error->message : "?" );
            if ( error )
                g_error_free( error );
        }
    }
};

KWPEAdBlock* KWPEAdBlock::self()
{
    // Never deleted: GLib callbacks may still come (the module is never
    // unloaded either).
    if ( !s_self )
        s_self = new KWPEAdBlock;
    return s_self;
}

KWPEAdBlock::KWPEAdBlock()
    : QObject( 0, "KWPEAdBlock" )
{
    readConfig();
    s.store = webkit_user_content_filter_store_new( QFile::encodeName( storeDir() ).data() );
    if ( !s.filterIds.isEmpty() )
        loadFilters( s.filterIds, false );
    QTimer* timer = new QTimer( this );
    connect( timer, SIGNAL( timeout() ), SLOT( checkSchedule() ) );
    timer->start( 3600 * 1000 );
    // The first time at once (nothing to block with yet), else a little
    // after the start (it runs in the background, but downloads compete
    // with the first page).
    QTimer::singleShot( s.filterIds.isEmpty() ? 0 : 30000, this, SLOT( checkSchedule() ) );
}

void KWPEAdBlock::readConfig()
{
    KConfig* c = KWPESettings::config();
    c->setGroup( "AdBlock" );
    s.enabled = c->readBoolEntry( "Enabled", true );
    QStringList enabled = c->readListEntry( "EnabledLists" );
    if ( !c->hasKey( "EnabledLists" ) )
        for ( unsigned i = 0; i < builtinCount; ++i )
            if ( builtinLists[i].enabled )
                enabled << builtinLists[i].id;
    s.lists.clear();
    for ( unsigned i = 0; i < builtinCount; ++i ) {
        List l;
        l.id = builtinLists[i].id;
        l.name = i18n( builtinLists[i].name );
        l.url = builtinLists[i].url;
        l.enabled = enabled.contains( l.id );
        l.custom = false;
        s.lists.append( l );
    }
    QStringList custom = c->readListEntry( "CustomLists" );
    for ( QStringList::ConstIterator it = custom.begin(); it != custom.end(); ++it ) {
        if ( !isAllowedURL( KURL( *it ) ) )
            continue;
        List l;
        l.id = customId( *it );
        l.name = *it;
        l.url = *it;
        l.enabled = enabled.contains( l.id );
        l.custom = true;
        s.lists.append( l );
    }
    s.sites.clear();
    QStringList sites = c->readListEntry( "DisabledSites" );
    for ( QStringList::ConstIterator it = sites.begin(); it != sites.end(); ++it )
        if ( !normalizedHost( *it ).isEmpty() )
            s.sites.append( normalizedHost( *it ) );
    s.filterIds = c->readListEntry( "Filters" );
    s.lastUpdate = readTime( c, "LastUpdate" );
    s.lastAttempt = readTime( c, "LastAttempt" );
    s.lastResult = c->readEntry( "LastResult" );
    s.rules = c->readUnsignedNumEntry( "Rules", 0 );
}

void KWPEAdBlock::reloadConfig()
{
    readConfig();
    // Another process finished an update: its set.
    if ( !s.updating && s.filterIds != s.filtersIds && s.filterIds != s.loadIds )
        loadFilters( s.filterIds, false );
    applyToViews();
    emit changed();
}

// ---------------------------------------------------------------------
// views

void KWPEAdBlock::addView( WebKitUserContentManager* manager )
{
    s.viewHost[manager] = QString::null;
    s.viewSet[manager] = 0;
    apply( manager );
}

void KWPEAdBlock::removeView( WebKitUserContentManager* manager )
{
    s.viewHost.remove( manager );
    s.viewSet.remove( manager );
}

bool KWPEAdBlock::setViewHost( WebKitUserContentManager* manager, const QString& host )
{
    if ( !s.viewHost.contains( manager ) )
        return false;
    s.viewHost[manager] = normalizedHost( host );
    return apply( manager );
}

bool KWPEAdBlock::apply( WebKitUserContentManager* manager )
{
    unsigned wanted = isBlocking( s.viewHost[manager] ) ? s.setSerial : 0;
    unsigned had = s.viewSet[manager];
    if ( had == wanted )
        return false;
    webkit_user_content_manager_remove_all_filters( manager );
    for ( QValueList<WebKitUserContentFilter*>::ConstIterator it = s.filters.begin(); wanted && it != s.filters.end(); ++it )
        webkit_user_content_manager_add_filter( manager, *it );
    s.viewSet[manager] = wanted;
    return !had != !wanted;
}

void KWPEAdBlock::applyToViews()
{
    for ( QMap<WebKitUserContentManager*, QString>::ConstIterator it = s.viewHost.begin(); it != s.viewHost.end(); ++it )
        apply( it.key() );
}

// ---------------------------------------------------------------------
// settings

bool KWPEAdBlock::isEnabled() const
{
    return s.enabled;
}

void KWPEAdBlock::setEnabled( bool enabled )
{
    s.enabled = enabled;
    KConfig* c = KWPESettings::config();
    c->setGroup( "AdBlock" );
    c->writeEntry( "Enabled", enabled );
}

bool KWPEAdBlock::isSiteDisabled( const QString& host ) const
{
    QString h = normalizedHost( host );
    for ( QStringList::ConstIterator it = s.sites.begin(); it != s.sites.end(); ++it )
        if ( hostMatches( h, *it ) )
            return true;
    return false;
}

void KWPEAdBlock::setSiteDisabled( const QString& host, bool disabled )
{
    QString h = normalizedHost( host );
    if ( h.isEmpty() )
        return;
    if ( disabled ) {
        if ( isSiteDisabled( h ) )
            return;
        s.sites.append( h );
    } else {
        // every entry that covers the host (a parent domain too)
        QStringList keep;
        for ( QStringList::ConstIterator it = s.sites.begin(); it != s.sites.end(); ++it )
            if ( !hostMatches( h, *it ) )
                keep.append( *it );
        s.sites = keep;
    }
    setDisabledSites( s.sites );
    KWPESettings::changed();
}

QStringList KWPEAdBlock::disabledSites() const
{
    return s.sites;
}

void KWPEAdBlock::setDisabledSites( const QStringList& sites )
{
    QStringList clean;
    for ( QStringList::ConstIterator it = sites.begin(); it != sites.end(); ++it ) {
        QString h = normalizedHost( *it );
        if ( !h.isEmpty() && !clean.contains( h ) )
            clean.append( h );
    }
    s.sites = clean;
    KConfig* c = KWPESettings::config();
    c->setGroup( "AdBlock" );
    c->writeEntry( "DisabledSites", clean );
}

QValueList<KWPEAdBlock::List> KWPEAdBlock::lists() const
{
    return s.lists;
}

void KWPEAdBlock::setLists( const QValueList<List>& lists )
{
    QStringList enabled, custom;
    for ( QValueList<List>::ConstIterator it = lists.begin(); it != lists.end(); ++it ) {
        List l = *it;
        if ( l.custom ) {
            if ( !isAllowedURL( KURL( l.url ) ) || custom.contains( l.url ) )
                continue;
            custom.append( l.url );
            l.id = customId( l.url );
        }
        if ( l.enabled )
            enabled.append( l.id );
    }
    KConfig* c = KWPESettings::config();
    c->setGroup( "AdBlock" );
    c->writeEntry( "EnabledLists", enabled );
    c->writeEntry( "CustomLists", custom );
    readConfig();
}

QDateTime KWPEAdBlock::lastUpdate() const
{
    return s.lastUpdate;
}

bool KWPEAdBlock::isUpdating() const
{
    return s.updating;
}

QString KWPEAdBlock::status() const
{
    if ( s.updating )
        return s.status;
    QString text;
    if ( !s.filters.isEmpty() )
        text = i18n( "%1 rules, updated %2." ).arg( KGlobal::locale()->formatNumber( s.rules, 0 ) )
                   .arg( KGlobal::locale()->formatDateTime( s.lastUpdate ) );
    else
        text = i18n( "No filters yet." );
    if ( !s.status.isEmpty() )
        text += " " + s.status;
    return text;
}

bool KWPEAdBlock::isBlocking( const QString& host ) const
{
    return s.enabled && !s.filters.isEmpty() && !isSiteDisabled( host );
}

bool KWPEAdBlock::isReady() const
{
    // loadIds: a set is being read (at the start, or after an update)
    return !s.enabled || s.loadIds.isEmpty() || !s.filters.isEmpty();
}

void KWPEAdBlock::setStatus( const QString& status )
{
    s.status = status;
    emit changed();
}

// ---------------------------------------------------------------------
// filters

void KWPEAdBlock::loadFilters( const QStringList& ids, bool fromUpdate )
{
    ++s.loadSerial;
    for ( QValueList<WebKitUserContentFilter*>::Iterator it = s.loading.begin(); it != s.loading.end(); ++it )
        webkit_user_content_filter_unref( *it );
    s.loading.clear();
    s.loadIds = ids;
    s.loadStart = g_get_monotonic_time();
    s.loadFailed = false;
    s.loadFromUpdate = fromUpdate;
    s.loadPending = ids.count();
    for ( QStringList::ConstIterator it = ids.begin(); it != ids.end(); ++it )
        webkit_user_content_filter_store_load( s.store, ( *it ).utf8().data(), 0, KWPEAdBlockGlue::loaded,
                                               GUINT_TO_POINTER( s.loadSerial ) );
}

void KWPEAdBlock::filtersLoaded( unsigned )
{
    bool fromUpdate = s.loadFromUpdate;
    QStringList ids = s.loadIds;
    s.loadIds.clear();
    if ( s.loadFailed ) {
        for ( QValueList<WebKitUserContentFilter*>::Iterator it = s.loading.begin(); it != s.loading.end(); ++it )
            webkit_user_content_filter_unref( *it );
        s.loading.clear();
        if ( fromUpdate ) {
            finishUpdate( false, i18n( "The compiled filters could not be loaded." ) );
        } else {
            if ( !s.updating ) {
                // The store lost them (deleted, another WebKit version
                // that does not read them): compile anew, now.
                s.filterIds.clear();
                s.lastUpdate = QDateTime();
                s.lastAttempt = QDateTime();
                QTimer::singleShot( 0, this, SLOT( checkSchedule() ) );
            }
            emit changed();
        }
        return;
    }

    if ( getenv( "KWPE_STATS" ) )
        fprintf( stderr, "kwpe: %u content filters ready after %.0f ms\n", unsigned( s.loading.count() ),
                 ( g_get_monotonic_time() - s.loadStart ) / 1000.0 );

    // The new set: every view gets it, then the old one goes.
    QValueList<WebKitUserContentFilter*> old = s.filters;
    s.filters = s.loading;
    s.filtersIds = ids;
    s.loading.clear();
    ++s.setSerial;
    for ( QMap<WebKitUserContentManager*, unsigned>::Iterator it = s.viewSet.begin(); it != s.viewSet.end(); ++it )
        it.data() = unsigned( -1 );   // whatever it has: not the new set
    applyToViews();
    for ( QValueList<WebKitUserContentFilter*>::Iterator it = old.begin(); it != old.end(); ++it )
        webkit_user_content_filter_unref( *it );

    if ( fromUpdate ) {
        // In use from now on, here and (after changed()) in the other
        // processes.  Then the old set leaves the store.
        s.filterIds = ids;
        s.rules = s.newRules;
        KConfig* c = KWPESettings::config();
        c->setGroup( "AdBlock" );
        c->writeEntry( "Filters", ids );
        c->writeEntry( "ListsHash", s.newHash );
        c->writeEntry( "Rules", s.rules );
        c->writeEntry( "Statistics", s.newStats );
        c->sync();
        webkit_user_content_filter_store_fetch_identifiers( s.store, 0, KWPEAdBlockGlue::identifiers, 0 );
    } else {
        emit changed();
    }
}

// ---------------------------------------------------------------------
// updating

void KWPEAdBlock::checkSchedule()
{
    if ( !s.enabled || s.updating )
        return;
    QDateTime now = QDateTime::currentDateTime();
    bool due = s.filterIds.isEmpty() || !s.lastUpdate.isValid() || s.lastUpdate.addDays( updateIntervalDays ) < now;
    // not again and again after a failure (every process, every hour)
    if ( due && s.lastAttempt.isValid() && s.lastAttempt.addSecs( retryMinutes * 60 ) > now && s.lastAttempt <= now )
        due = false;
    if ( due )
        updateNow();
}

void KWPEAdBlock::updateNow()
{
    if ( s.updating )
        return;
    // One process at a time (Konqueror may run several).
    // A lock older than an update can take (a crashed process whose PID
    // was given to another one, another host): gone.
    QString lockPath = baseDir() + "update.lock";
    QFileInfo lockInfo( lockPath );
    if ( lockInfo.exists() && lockInfo.lastModified().addSecs( 1800 ) < QDateTime::currentDateTime() )
        QFile::remove( lockPath );
    s.lock = new KLockFile( lockPath );
    s.lock->setStaleTime( 1800 );
    // With LockNoBlock a first look only reads who holds the lock; the
    // second one takes it over if that process is gone.
    KLockFile::LockResult locked = s.lock->lock( KLockFile::LockNoBlock | KLockFile::LockForce );
    if ( locked == KLockFile::LockFail )
        locked = s.lock->lock( KLockFile::LockNoBlock | KLockFile::LockForce );
    if ( locked != KLockFile::LockOK ) {
        s.lock = 0;
        setStatus( i18n( "Another window is updating the filter lists." ) );
        return;
    }
    s.updating = true;
    s.errors.clear();
    s.files.clear();
    s.queue.clear();
    s.includes = 0;
    s.newPrefix = QString( filterPrefix ) + QString::number( QDateTime::currentDateTime().toTime_t() ) + "p"
                  + QString::number( getpid() );
    if ( s.filterIds.grep( s.newPrefix + "-" ).count() )
        s.newPrefix += "b";
    s.lastAttempt = QDateTime::currentDateTime();
    KConfig* c = KWPESettings::config();
    c->setGroup( "AdBlock" );
    c->writeEntry( "LastAttempt", s.lastAttempt.toTime_t() );
    c->sync();

    for ( QValueList<List>::ConstIterator it = s.lists.begin(); it != s.lists.end(); ++it ) {
        if ( !( *it ).enabled )
            continue;
        Download d;
        d.listId = ( *it ).id;
        d.listName = ( *it ).name;
        d.url = KURL( ( *it ).url );
        d.path = listDir( d.listId ) + "list.txt";
        d.include = false;
        s.queue.append( d );
    }
    downloadNext();
}

void KWPEAdBlock::downloadNext()
{
    if ( s.queue.isEmpty() ) {
        convert();
        return;
    }
    s.current = s.queue.first();
    s.queue.remove( s.queue.begin() );
    s.data = QByteArray();
    s.total = 0;
    if ( !isAllowedURL( s.current.url ) ) {
        s.errors.append( i18n( "%1: neither an https URL nor a local file" ).arg( s.current.listName ) );
        downloadNext();
        return;
    }
    setStatus( i18n( "Downloading %1..." ).arg( s.current.include ? s.current.url.fileName() : s.current.listName ) );
    // Anonymous: no cookies, no language, no cache; a certificate that
    // does not validate fails the job (no dialog); HTTP errors are errors.
    KIO::TransferJob* job = KIO::get( s.current.url, true, false );
    job->addMetaData( "errorPage", "false" );
    job->addMetaData( "cookies", "none" );
    job->addMetaData( "ssl_militant", "TRUE" );
    job->addMetaData( "SendLanguageSettings", "false" );
    job->addMetaData( "no-auth", "true" );
    s.job = job;
    connect( job, SIGNAL( data( KIO::Job*, const QByteArray& ) ), SLOT( slotData( KIO::Job*, const QByteArray& ) ) );
    connect( job, SIGNAL( redirection( KIO::Job*, const KURL& ) ), SLOT( slotRedirection( KIO::Job*, const KURL& ) ) );
    connect( job, SIGNAL( result( KIO::Job* ) ), SLOT( slotResult( KIO::Job* ) ) );
    connect( job, SIGNAL( totalSize( KIO::Job*, KIO::filesize_t ) ), SLOT( slotTotalSize( KIO::Job*, KIO::filesize_t ) ) );
}

void KWPEAdBlock::slotData( KIO::Job* job, const QByteArray& data )
{
    if ( job != s.job || data.isEmpty() )
        return;
    unsigned size = s.data.size();
    if ( size + data.size() > maxListSize ) {
        s.job = 0;
        job->kill( true );
        s.errors.append( i18n( "%1: larger than %2 MiB" ).arg( s.current.listName ).arg( maxListSize >> 20 ) );
        downloadNext();
        return;
    }
    s.data.resize( size + data.size() );
    memcpy( s.data.data() + size, data.data(), data.size() );
}

void KWPEAdBlock::slotTotalSize( KIO::Job* job, KIO::filesize_t size )
{
    if ( job == s.job )
        s.total = size;
}

void KWPEAdBlock::slotRedirection( KIO::Job* job, const KURL& url )
{
    if ( job != s.job )
        return;
    // https only; an included file stays on the host of its list
    if ( url.protocol() == "https" && isAllowedURL( url ) && ( !s.current.include || url.host() == s.current.url.host() ) )
        return;
    s.job = 0;
    job->kill( true );
    s.errors.append( i18n( "%1: redirected to %2" ).arg( s.current.listName ).arg( url.prettyURL() ) );
    downloadNext();
}

void KWPEAdBlock::slotResult( KIO::Job* job )
{
    if ( job != s.job )
        return;
    s.job = 0;
    bool ok = !job->error();
    QString text;
    if ( ok ) {
        // A filter list is text; an HTML page (a captive portal, an error
        // page) or binary data is not one.
        for ( unsigned i = 0; ok && i < s.data.size(); ++i )
            if ( s.data[i] == 0 )
                ok = false;
        // All of it: a body cut short (a connection closed early, no
        // length) would end in half a filter, which blocks more.
        if ( s.total && KIO::filesize_t( s.data.size() ) < s.total )
            ok = false;
        text = QString::fromUtf8( s.data.data(), s.data.size() );
        if ( ok && !text.endsWith( "\n" ) ) {
            int last = text.findRev( '\n' );
            text.truncate( last < 0 ? 0 : last + 1 );
            s.data.resize( text.utf8().length() );
            memcpy( s.data.data(), text.utf8().data(), s.data.size() );
        }
        // a filter list starts with its header ("[Adblock Plus 2.0]",
        // "! Title: ..."), not with HTML (a captive portal)
        QString head = text.left( 512 ).stripWhiteSpace();
        if ( !( head.startsWith( "[" ) || head.startsWith( "!" ) ) || head.lower().contains( "<html" ) )
            ok = false;
        if ( !ok )
            s.errors.append( i18n( "%1: not a filter list" ).arg( s.current.listName ) );
    } else {
        s.errors.append( i18n( "%1: %2" ).arg( s.current.listName ).arg( job->errorString() ) );
    }

    if ( ok ) {
        KSaveFile file( s.current.path, 0600 );
        if ( file.status() == 0 && file.file() ) {
            file.file()->writeBlock( s.data.data(), s.data.size() );
            if ( !file.close() )
                ok = false;
        } else {
            ok = false;
        }
        if ( !ok )
            s.errors.append( i18n( "%1: cannot write %2" ).arg( s.current.listName ).arg( s.current.path ) );
    }

    if ( ok && !s.current.include ) {
        // uBlock's "!#include file": next to the list, same host.
        QStringList lines = QStringList::split( '\n', text );
        QRegExp name( "^[A-Za-z0-9][A-Za-z0-9._-]*$" );
        QValueList<Download> includes;
        for ( QStringList::ConstIterator it = lines.begin(); it != lines.end(); ++it ) {
            if ( !( *it ).startsWith( "!#include " ) )
                continue;
            QString file = ( *it ).mid( 10 ).stripWhiteSpace();
            if ( !name.exactMatch( file ) || ++s.includes > maxIncludes )
                continue;
            Download d = s.current;
            d.url = KURL( s.current.url, file );
            d.path = listDir( d.listId ) + file;
            d.include = true;
            if ( d.url.host() == s.current.url.host() && isAllowedURL( d.url ) && file != "list.txt" )
                includes.append( d );
        }
        // next in the queue, in their order
        for ( int i = int( includes.count() ) - 1; i >= 0; --i )
            s.queue.prepend( includes[i] );
    }
    // A list that could not be fetched: the last copy, if there is one.
    if ( !s.current.include && QFile::exists( s.current.path ) )
        s.files.append( s.current.path );
    downloadNext();
}

void KWPEAdBlock::convert()
{
    if ( s.files.isEmpty() ) {
        finishUpdate( false, s.errors.isEmpty() ? i18n( "No filter list is enabled." ) : s.errors.join( "; " ) );
        return;
    }
    // Nothing new (lists, includes and the choice are the same): the set
    // in use stays.
    KMD5 md5;
    for ( QStringList::ConstIterator it = s.files.begin(); it != s.files.end(); ++it ) {
        md5.update( QFile::encodeName( *it ) );
        QDir dir( ( *it ).left( ( *it ).findRev( '/' ) ) );
        QStringList entries = dir.entryList( QDir::Files, QDir::Name );
        for ( QStringList::ConstIterator e = entries.begin(); e != entries.end(); ++e ) {
            QFile f( dir.filePath( *e ) );
            if ( f.open( IO_ReadOnly ) )
                md5.update( f );
        }
    }
    md5.update( QCString( "kwpe-filterconv 1" ) );
    s.newHash = QString::fromLatin1( md5.hexDigest() );
    KConfig* c = KWPESettings::config();
    c->setGroup( "AdBlock" );
    if ( s.newHash == c->readEntry( "ListsHash" ) && !s.filters.isEmpty() && s.filtersIds == s.filterIds ) {
        finishUpdate( true, QString::null );
        return;
    }

    QString exe = KStandardDirs::findExe( "kwpe-filterconv" );
    if ( exe.isEmpty() ) {
        finishUpdate( false, i18n( "kwpe-filterconv is not installed." ) );
        return;
    }
    setStatus( i18n( "Compiling the filters..." ) );
    s.output = QCString();
    s.newIds.clear();
    s.process = new KProcess( this );
    *s.process << exe << "--store" << storeDir() << "--id" << s.newPrefix;
    for ( QStringList::ConstIterator it = s.files.begin(); it != s.files.end(); ++it )
        *s.process << *it;
    s.process->setPriority( 10 );
    connect( s.process, SIGNAL( receivedStdout( KProcess*, char*, int ) ), SLOT( slotConverterOutput( KProcess*, char*, int ) ) );
    connect( s.process, SIGNAL( processExited( KProcess* ) ), SLOT( slotConverterExited( KProcess* ) ) );
    if ( !s.process->start( KProcess::NotifyOnExit, KProcess::Stdout ) ) {
        delete s.process;
        s.process = 0;
        finishUpdate( false, i18n( "kwpe-filterconv could not be started." ) );
    }
}

void KWPEAdBlock::slotConverterOutput( KProcess*, char* buffer, int length )
{
    if ( s.output.length() < 1024 * 1024 )
        s.output += QCString( buffer, length + 1 );
}

void KWPEAdBlock::slotConverterExited( KProcess* process )
{
    bool ok = process->normalExit() && process->exitStatus() == 0;
    s.process = 0;
    process->deleteLater();

    QStringList lines = QStringList::split( '\n', QString::fromLocal8Bit( s.output ) );
    QString error;
    s.newRules = 0;
    s.newStats = QString::null;
    unsigned dropped = 0;
    for ( QStringList::ConstIterator it = lines.begin(); it != lines.end(); ++it ) {
        QStringList f = QStringList::split( ' ', *it );
        if ( f.count() >= 3 && f[0] == "filter" && f[1].startsWith( s.newPrefix + "-" ) ) {
            s.newIds.append( f[1] );
            s.newRules += f[2].toUInt();
        } else if ( f.count() >= 2 && f[0] == "error" ) {
            error = ( *it ).mid( 6 );
        } else if ( f.count() == 3 && f[0] == "stats" && f[1] == "dropped" ) {
            dropped = f[2].toUInt();
        } else if ( f.count() == 3 && f[0] == "time" ) {
            s.newStats += QString( "%1 %2 ms; " ).arg( f[1] ).arg( f[2] );
        }
    }
    s.newStats += QString( "dropped %1" ).arg( dropped );
    if ( !ok || s.newIds.isEmpty() ) {
        finishUpdate( false, error.isEmpty() ? i18n( "kwpe-filterconv failed." ) : error );
        return;
    }
    setStatus( i18n( "Loading the filters..." ) );
    loadFilters( s.newIds, true );
}

void KWPEAdBlock::finishUpdate( bool ok, const QString& message )
{
    s.updating = false;
    if ( s.lock ) {
        s.lock->unlock();
        s.lock = 0;
    }
    KConfig* c = KWPESettings::config();
    c->setGroup( "AdBlock" );
    // A download that failed (offline) is tried again in an hour, not in
    // a week, even though the last copies were used.
    if ( ok && s.errors.isEmpty() ) {
        s.lastUpdate = QDateTime::currentDateTime();
        c->writeEntry( "LastUpdate", s.lastUpdate.toTime_t() );
    }
    s.lastResult = !s.errors.isEmpty() ? s.errors.join( "; " ) : message;
    c->writeEntry( "LastResult", s.lastResult );
    s.status = s.lastResult;
    if ( !ok && s.status.isEmpty() )
        s.status = i18n( "The update failed." );
    // here and in the other processes (they load the new set)
    KWPESettings::changed();
}

#include "kwpeadblock.moc"

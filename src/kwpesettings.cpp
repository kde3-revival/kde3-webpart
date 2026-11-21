/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#include <qcstring.h>

#include <dcopclient.h>
#include <dcopobject.h>
#include <kapplication.h>
#include <kconfig.h>

#include "kwpeadblock.h"
#include "kwpesettings.h"
#include "kwpewidget.h"

namespace
{

const char dcopObject[] = "KWPEPartSettings";
const char dcopSignal[] = "changed()";
const char dcopSlot[] = "reparse()";

// Receives the "changed()" signal of any process with the part (this one
// too, which costs a reread of a small file).
class SettingsDCOP : public DCOPObject
{
public:
    SettingsDCOP() : DCOPObject( dcopObject ) {}

    virtual bool process( const QCString& fun, const QByteArray& data, QCString& replyType, QByteArray& replyData )
    {
        if ( fun == dcopSlot ) {
            replyType = "void";
            KWPESettings::config()->reparseConfiguration();
            KWPESettings::apply();
            return true;
        }
        return DCOPObject::process( fun, data, replyType, replyData );
    }

    virtual QCStringList functions()
    {
        QCStringList list = DCOPObject::functions();
        list << QCString( "void " ) + dcopSlot;
        return list;
    }
};

KConfig* s_config = 0;
SettingsDCOP* s_dcop = 0;

}

KConfig* KWPESettings::config()
{
    if ( !s_config ) {
        s_config = new KConfig( "kwpepartrc" );
        DCOPClient* client = kapp ? kapp->dcopClient() : 0;
        if ( client && ( client->isAttached() || client->attach() ) ) {
            s_dcop = new SettingsDCOP;
            client->connectDCOPSignal( 0, dcopObject, dcopSignal, dcopObject, dcopSlot, false );
        }
    }
    return s_config;
}

KWPESettings::SmoothScrolling KWPESettings::smoothScrolling()
{
    KConfig* c = config();
    c->setGroup( "Settings" );
    QString value;
    if ( c->hasKey( "SmoothScrolling" ) ) {
        value = c->readEntry( "SmoothScrolling" );
    } else {
        KConfig khtml( "khtmlrc", true, false );
        khtml.setGroup( "KHTML Settings" );
        value = khtml.readEntry( "SmoothScrolling", "Never" );
    }
    if ( value == "Always" )
        return SmoothAlways;
    if ( value == "WhenEfficient" )
        return SmoothWhenEfficient;
    return SmoothNever;
}

void KWPESettings::setSmoothScrolling( SmoothScrolling mode )
{
    KConfig* c = config();
    c->setGroup( "Settings" );
    c->writeEntry( "SmoothScrolling", mode == SmoothAlways ? "Always" : mode == SmoothWhenEfficient ? "WhenEfficient" : "Never" );
}

bool KWPESettings::pdfInPage()
{
    KConfig* c = config();
    c->setGroup( "Settings" );
    return c->readBoolEntry( "PDFInPage", false );
}

void KWPESettings::setPDFInPage( bool inPage )
{
    KConfig* c = config();
    c->setGroup( "Settings" );
    c->writeEntry( "PDFInPage", inPage );
}

void KWPESettings::changed()
{
    config()->sync();
    apply();
    DCOPClient* client = kapp ? kapp->dcopClient() : 0;
    if ( client && client->isAttached() )
        client->emitDCOPSignal( dcopObject, dcopSignal, QByteArray() );
}

void KWPESettings::apply()
{
    KWPEWidget::applySettings();
    KWPEAdBlock::self()->reloadConfig();
}

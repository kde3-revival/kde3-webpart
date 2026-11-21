/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#include <qcheckbox.h>
#include <qcombobox.h>
#include <qguardedptr.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qlistbox.h>
#include <qlistview.h>
#include <qpushbutton.h>
#include <qwhatsthis.h>

#include <kdialog.h>
#include <klineedit.h>
#include <klocale.h>
#include <kmessagebox.h>
#include <kurl.h>

#include "kwpeadblock.h"
#include "kwpeconfigdialog.h"
#include "kwpesettings.h"

namespace
{

QGuardedPtr<KWPEConfigDialog> s_dialog;

// A filter list in the list view: its id and whether it is a custom one.
class ListItem : public QCheckListItem
{
public:
    ListItem( QListView* parent, const KWPEAdBlock::List& list )
        : QCheckListItem( parent, list.name, QCheckListItem::CheckBox ), m_list( list )
    {
        setText( 1, list.url );
        setOn( list.enabled );
    }

    KWPEAdBlock::List list() const
    {
        KWPEAdBlock::List l = m_list;
        l.enabled = isOn();
        return l;
    }

private:
    KWPEAdBlock::List m_list;
};

}

void KWPEConfigDialog::showDialog( QWidget* parent )
{
    if ( !s_dialog )
        s_dialog = new KWPEConfigDialog( parent );
    s_dialog->show();
    s_dialog->raise();
    s_dialog->setActiveWindow();
}

KWPEConfigDialog::KWPEConfigDialog( QWidget* parent )
    : KDialogBase( Tabbed, i18n( "Configure WPE WebKit" ), Ok | Apply | Cancel, Ok, parent, "kwpeconfig", false, true )
{
    setWFlags( getWFlags() | WDestructiveClose );

    // General
    QFrame* general = addPage( i18n( "&General" ) );
    QGridLayout* grid = new QGridLayout( general, 2, 2, 0, KDialog::spacingHint() );
    m_smooth = new QComboBox( false, general );
    m_smooth->insertItem( i18n( "Never" ) );
    m_smooth->insertItem( i18n( "When Efficient" ) );
    m_smooth->insertItem( i18n( "Always" ) );
    QLabel* label = new QLabel( m_smooth, i18n( "S&mooth scrolling:" ), general );
    grid->addWidget( label, 0, 0 );
    grid->addWidget( m_smooth, 0, 1 );
    grid->setColStretch( 1, 1 );
    grid->setRowStretch( 1, 1 );
    QString smoothHelp = i18n( "Whether the mouse wheel and the keyboard scroll with an animation. "
                               "\"When Efficient\": only when pages are drawn on the GPU and handed "
                               "to the X server without a copy." );
    QWhatsThis::add( m_smooth, smoothHelp );
    QWhatsThis::add( label, smoothHelp );

    // Ad blocking
    QFrame* ads = addPage( i18n( "&Ad Blocking" ) );
    QVBoxLayout* top = new QVBoxLayout( ads, 0, KDialog::spacingHint() );
    m_enabled = new QCheckBox( i18n( "&Block advertisements and trackers" ), ads );
    top->addWidget( m_enabled );

    top->addWidget( new QLabel( i18n( "Filter lists (Adblock Plus syntax, fetched over HTTPS):" ), ads ) );
    m_lists = new QListView( ads );
    m_lists->addColumn( i18n( "List" ) );
    m_lists->addColumn( i18n( "Address" ) );
    m_lists->setAllColumnsShowFocus( true );
    m_lists->setSorting( -1 );
    top->addWidget( m_lists, 2 );
    QHBoxLayout* row = new QHBoxLayout( top );
    m_url = new KLineEdit( ads );
    QWhatsThis::add( m_url, i18n( "The address of another filter list: https://... or a local file." ) );
    m_add = new QPushButton( i18n( "A&dd List" ), ads );
    m_remove = new QPushButton( i18n( "&Remove List" ), ads );
    row->addWidget( m_url, 1 );
    row->addWidget( m_add );
    row->addWidget( m_remove );

    row = new QHBoxLayout( top );
    m_status = new QLabel( ads );
    m_status->setAlignment( Qt::AlignVCenter | Qt::WordBreak );
    m_update = new QPushButton( i18n( "&Update Now" ), ads );
    row->addWidget( m_status, 1 );
    row->addWidget( m_update );

    top->addWidget( new QLabel( i18n( "Sites where ad blocking is off:" ), ads ) );
    row = new QHBoxLayout( top );
    m_sites = new QListBox( ads );
    m_removeSite = new QPushButton( i18n( "Re&move Site" ), ads );
    row->addWidget( m_sites, 1 );
    QVBoxLayout* col = new QVBoxLayout( row );
    col->addWidget( m_removeSite );
    col->addStretch( 1 );

    connect( m_add, SIGNAL( clicked() ), SLOT( addList() ) );
    connect( m_url, SIGNAL( returnPressed() ), SLOT( addList() ) );
    connect( m_remove, SIGNAL( clicked() ), SLOT( removeList() ) );
    connect( m_lists, SIGNAL( selectionChanged() ), SLOT( listSelected() ) );
    connect( m_removeSite, SIGNAL( clicked() ), SLOT( removeSite() ) );
    connect( m_update, SIGNAL( clicked() ), SLOT( updateNow() ) );
    connect( KWPEAdBlock::self(), SIGNAL( changed() ), SLOT( updateStatus() ) );

    load();
    resize( 640, 520 );
}

void KWPEConfigDialog::load()
{
    KWPESettings::SmoothScrolling smooth = KWPESettings::smoothScrolling();
    m_smooth->setCurrentItem( smooth == KWPESettings::SmoothAlways ? 2 : smooth == KWPESettings::SmoothWhenEfficient ? 1 : 0 );

    KWPEAdBlock* adblock = KWPEAdBlock::self();
    m_enabled->setChecked( adblock->isEnabled() );
    m_lists->clear();
    QValueList<KWPEAdBlock::List> lists = adblock->lists();
    QListViewItem* last = 0;
    for ( QValueList<KWPEAdBlock::List>::ConstIterator it = lists.begin(); it != lists.end(); ++it ) {
        ListItem* item = new ListItem( m_lists, *it );
        if ( last )
            item->moveItem( last );
        last = item;
    }
    m_sites->clear();
    m_sites->insertStringList( adblock->disabledSites() );
    listSelected();
    updateStatus();
}

void KWPEConfigDialog::save()
{
    int smooth = m_smooth->currentItem();
    KWPESettings::setSmoothScrolling( smooth == 2 ? KWPESettings::SmoothAlways
                                      : smooth == 1 ? KWPESettings::SmoothWhenEfficient : KWPESettings::SmoothNever );

    KWPEAdBlock* adblock = KWPEAdBlock::self();
    QValueList<KWPEAdBlock::List> before = adblock->lists();
    QValueList<KWPEAdBlock::List> lists;
    for ( QListViewItem* item = m_lists->firstChild(); item; item = item->nextSibling() )
        lists.append( static_cast<ListItem*>( item )->list() );
    bool wasEnabled = adblock->isEnabled();
    adblock->setEnabled( m_enabled->isChecked() );
    adblock->setLists( lists );
    QStringList sites;
    for ( unsigned i = 0; i < m_sites->count(); ++i )
        sites.append( m_sites->text( i ) );
    adblock->setDisabledSites( sites );
    KWPESettings::changed();

    // Another choice of lists: compile them now.
    QStringList enabledBefore, enabledNow;
    for ( QValueList<KWPEAdBlock::List>::ConstIterator it = before.begin(); it != before.end(); ++it )
        if ( ( *it ).enabled )
            enabledBefore.append( ( *it ).url );
    QValueList<KWPEAdBlock::List> after = adblock->lists();
    for ( QValueList<KWPEAdBlock::List>::ConstIterator it = after.begin(); it != after.end(); ++it )
        if ( ( *it ).enabled )
            enabledNow.append( ( *it ).url );
    if ( adblock->isEnabled() && ( enabledBefore != enabledNow || !wasEnabled ) )
        adblock->updateNow();
}

void KWPEConfigDialog::slotOk()
{
    save();
    accept();
}

void KWPEConfigDialog::slotApply()
{
    save();
}

void KWPEConfigDialog::updateStatus()
{
    KWPEAdBlock* adblock = KWPEAdBlock::self();
    m_status->setText( adblock->status() );
    m_update->setEnabled( !adblock->isUpdating() );
}

void KWPEConfigDialog::addList()
{
    QString text = m_url->text().stripWhiteSpace();
    if ( text.isEmpty() )
        return;
    KURL url = KURL::fromPathOrURL( text );
    if ( !url.isValid() || !( url.protocol() == "https" || url.isLocalFile() ) ) {
        KMessageBox::sorry( this, i18n( "Filter lists are fetched over HTTPS (or read from a local file); "
                                        "\"%1\" is neither." ).arg( text ) );
        return;
    }
    KWPEAdBlock::List l;
    l.id = QString::null;   // given when saved
    l.name = url.prettyURL();
    l.url = url.url();
    l.enabled = true;
    l.custom = true;
    QListViewItem* last = m_lists->lastItem();
    ListItem* item = new ListItem( m_lists, l );
    if ( last )
        item->moveItem( last );
    m_url->clear();
}

void KWPEConfigDialog::listSelected()
{
    ListItem* item = static_cast<ListItem*>( m_lists->selectedItem() );
    m_remove->setEnabled( item && item->list().custom );
}

void KWPEConfigDialog::removeList()
{
    ListItem* item = static_cast<ListItem*>( m_lists->selectedItem() );
    if ( item && item->list().custom )
        delete item;
    listSelected();
}

void KWPEConfigDialog::removeSite()
{
    int current = m_sites->currentItem();
    if ( current >= 0 )
        m_sites->removeItem( current );
}

void KWPEConfigDialog::updateNow()
{
    save();
    KWPEAdBlock::self()->updateNow();
    updateStatus();
}

#include "kwpeconfigdialog.moc"

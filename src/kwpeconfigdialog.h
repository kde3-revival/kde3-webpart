/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPECONFIGDIALOG_H
#define KWPECONFIGDIALOG_H

#include <kdialogbase.h>

class KLineEdit;
class QCheckBox;
class QComboBox;
class QLabel;
class QListBox;
class QListView;
class QPushButton;

/**
 * The part's settings: smooth scrolling, the ad blocker (on/off, filter
 * lists, update, sites where it is off).  Settings menu, "Configure WPE
 * WebKit...".  One instance (non-modal).
 */
class KWPEConfigDialog : public KDialogBase
{
    Q_OBJECT

public:
    static void showDialog( QWidget* parent );

protected slots:
    virtual void slotOk();
    virtual void slotApply();

private slots:
    void updateStatus();
    void addList();
    void removeList();
    void listSelected();
    void removeSite();
    void updateNow();

private:
    KWPEConfigDialog( QWidget* parent );
    void load();
    void save();

    QComboBox* m_smooth;
    QCheckBox* m_pdfInPage;
    QCheckBox* m_enabled;
    QCheckBox* m_scriptlets;
    QComboBox* m_interval;
    QListView* m_lists;
    KLineEdit* m_url;
    QPushButton* m_add;
    QPushButton* m_remove;
    QLabel* m_status;
    QPushButton* m_update;
    QListBox* m_sites;
    QPushButton* m_removeSite;
};

#endif

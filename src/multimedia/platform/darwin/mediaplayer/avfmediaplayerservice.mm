/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd and/or its subsidiary(-ies).
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "avfmediaplayerservice_p.h"
#include "avfmediaplayersession_p.h"
#include "avfmediaplayercontrol_p.h"
#include "avfvideooutput_p.h"
#if QT_CONFIG(opengl)
#include "avfvideorenderercontrol_p.h"
#endif
#include "avfvideowindowcontrol_p.h"

#import <AVFoundation/AVFoundation.h>

QT_USE_NAMESPACE

AVFMediaPlayerService::AVFMediaPlayerService()
{
    m_session = new AVFMediaPlayerSession(this);
    m_control = new AVFMediaPlayerControl(this);
    m_control->setSession(m_session);
}

AVFMediaPlayerService::~AVFMediaPlayerService()
{
#ifdef QT_DEBUG_AVF
    qDebug() << Q_FUNC_INFO;
#endif
    delete m_session;
    //delete m_control;
}

QMediaPlayerControl *AVFMediaPlayerService::player()
{
    return m_control;
}

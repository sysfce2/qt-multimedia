// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only
#ifndef QMOCKAUDIOOUTPUT_H
#define QMOCKAUDIOOUTPUT_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <private/qplatformaudiooutput_p.h>

QT_BEGIN_NAMESPACE

class QMockAudioOutput : public QPlatformAudioOutput
{
public:
    QMockAudioOutput(QAudioOutput *qq) : QPlatformAudioOutput(qq) {}
};

QT_END_NAMESPACE


#endif // QMOCKAUDIOOUTPUT_H

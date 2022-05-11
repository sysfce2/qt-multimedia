/****************************************************************************
**
** Copyright (C) 2022 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:COMM$
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** $QT_END_LICENSE$
**
**
**
**
**
**
**
**
**
**
**
**
**
**
**
**
**
**
**
******************************************************************************/


#ifndef QAUDIODEVICEINFO_H
#define QAUDIODEVICEINFO_H

#include <QtCore/qobject.h>
#include <QtCore/qbytearray.h>
#include <QtCore/qstring.h>
#include <QtCore/qstringlist.h>
#include <QtCore/qlist.h>

#include <QtMultimedia/qtmultimediaglobal.h>

#include <QtMultimedia/qaudio.h>
#include <QtMultimedia/qaudioformat.h>

QT_BEGIN_NAMESPACE

class QAudioDevicePrivate;
QT_DECLARE_QESDP_SPECIALIZATION_DTOR_WITH_EXPORT(QAudioDevicePrivate, Q_MULTIMEDIA_EXPORT)

class Q_MULTIMEDIA_EXPORT QAudioDevice
{
    Q_GADGET
    Q_PROPERTY(QByteArray id READ id CONSTANT)
    Q_PROPERTY(QString description READ description CONSTANT)
    Q_PROPERTY(bool isDefault READ isDefault CONSTANT)
    Q_PROPERTY(Mode mode READ mode CONSTANT)
public:
    enum Mode {
        Null,
        Input,
        Output
    };
    Q_ENUM(Mode)

    QAudioDevice();
    QAudioDevice(const QAudioDevice& other);
    ~QAudioDevice();

    QAudioDevice(QAudioDevice &&other) noexcept = default;
    QT_MOVE_ASSIGNMENT_OPERATOR_IMPL_VIA_PURE_SWAP(QAudioDevice)
    void swap(QAudioDevice &other) noexcept
    { d.swap(other.d); }

    QAudioDevice& operator=(const QAudioDevice& other);

    bool operator==(const QAudioDevice &other) const;
    bool operator!=(const QAudioDevice &other) const;

    bool isNull() const;

    QByteArray id() const;
    QString description() const;

    bool isDefault() const;
    QAudioDevice::Mode mode() const;

    bool isFormatSupported(const QAudioFormat &format) const;
    QAudioFormat preferredFormat() const;

    int minimumSampleRate() const;
    int maximumSampleRate() const;
    int minimumChannelCount() const;
    int maximumChannelCount() const;
    QList<QAudioFormat::SampleFormat> supportedSampleFormats() const;

    const QAudioDevicePrivate *handle() const { return d.get(); }
private:
    friend class QAudioDevicePrivate;
    QAudioDevice(QAudioDevicePrivate *p);
    QExplicitlySharedDataPointer<QAudioDevicePrivate> d;
};

#ifndef QT_NO_DEBUG_STREAM
Q_MULTIMEDIA_EXPORT QDebug operator<<(QDebug dbg, QAudioDevice::Mode mode);
#endif

QT_END_NAMESPACE

#endif // QAUDIODEVICEINFO_H

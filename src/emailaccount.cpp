/*
 * Copyright 2011 Intel Corporation.
 *
 * This program is licensed under the terms and conditions of the
 * Apache License, version 2.0.  The full text of the Apache License is at 	
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <QNetworkConfigurationManager>
#include <QTimer>

#include <qmailstore.h>
#include <qmailmessage.h>

#include "emailaccount.h"
#include "emailagent.h"

EmailAccount::EmailAccount()
    : mAccount(new QMailAccount())
    , mAccountConfig(new QMailAccountConfiguration())
    , mRecvCfg(0)
    , mSendCfg(0)
    , mRetrievalAction(new QMailRetrievalAction(this))
    , mTransmitAction(new QMailTransmitAction(this))
    , mErrorCode(0)
{ 
    EmailAgent::instance();
    mAccount->setMessageType(QMailMessage::Email);
    init();
}

EmailAccount::EmailAccount(const QMailAccount &other)
    : mAccount(new QMailAccount(other))
    , mAccountConfig(new QMailAccountConfiguration())
    , mRecvCfg(0)
    , mSendCfg(0)
    , mRetrievalAction(new QMailRetrievalAction(this))
    , mTransmitAction(new QMailTransmitAction(this))
    , mErrorCode(0)
{
    EmailAgent::instance();
    *mAccountConfig = QMailStore::instance()->accountConfiguration(mAccount->id());
    init();
}

EmailAccount::~EmailAccount()
{
    delete mRecvCfg;
    delete mSendCfg;
    delete mAccount;
}

void EmailAccount::init()
{
    QStringList services = mAccountConfig->services();
    if (!services.contains("qmfstoragemanager")) {
        // add qmfstoragemanager configuration
        mAccountConfig->addServiceConfiguration("qmfstoragemanager");
        QMailServiceConfiguration storageCfg(mAccountConfig, "qmfstoragemanager");
        storageCfg.setType(QMailServiceConfiguration::Storage);
        storageCfg.setVersion(101);
        storageCfg.setValue("basePath", "");
    }
    if (!services.contains("smtp")) {
        // add SMTP configuration
        mAccountConfig->addServiceConfiguration("smtp");
    }
    if (services.contains("imap4")) {
        mRecvType = "imap4";
    } else if (services.contains("pop3")) {
        mRecvType = "pop3";
    } else {
        // add POP configuration
        mRecvType = "pop3";
        mAccountConfig->addServiceConfiguration(mRecvType);
    }
    mSendCfg = new QMailServiceConfiguration(mAccountConfig, "smtp");
    mRecvCfg = new QMailServiceConfiguration(mAccountConfig, mRecvType);
    mSendCfg->setType(QMailServiceConfiguration::Sink);
    mSendCfg->setVersion(100);
    mRecvCfg->setType(QMailServiceConfiguration::Source);
    mRecvCfg->setVersion(100);

    connect(mRetrievalAction, SIGNAL(activityChanged(QMailServiceAction::Activity)), this, SLOT(activityChanged(QMailServiceAction::Activity)));
    connect(mTransmitAction, SIGNAL(activityChanged(QMailServiceAction::Activity)), this, SLOT(activityChanged(QMailServiceAction::Activity)));
}

void EmailAccount::clear()
{
    delete mAccount;
    delete mAccountConfig;
    mAccount = new QMailAccount();
    mAccountConfig = new QMailAccountConfiguration();
    mAccount->setMessageType(QMailMessage::Email);
    mPassword.clear();
    init();
}

bool EmailAccount::save()
{
    bool result;
    mAccount->setStatus(QMailAccount::UserEditable, true);
    mAccount->setStatus(QMailAccount::UserRemovable, true);
    mAccount->setStatus(QMailAccount::MessageSource, true);
    mAccount->setStatus(QMailAccount::CanRetrieve, true);
    mAccount->setStatus(QMailAccount::MessageSink, true);
    mAccount->setStatus(QMailAccount::CanTransmit, true);
    mAccount->setStatus(QMailAccount::Enabled, true);
    mAccount->setStatus(QMailAccount::CanCreateFolders, true);
    mAccount->setFromAddress(QMailAddress(address()));
    if (mAccount->id().isValid()) {
        result = QMailStore::instance()->updateAccount(mAccount, mAccountConfig);
    } else {
        if (preset() == noPreset) {
            // set description to server for custom email accounts
            setDescription(server());
        }
        result = QMailStore::instance()->addAccount(mAccount, mAccountConfig);
    }
    return result;
}

bool EmailAccount::remove()
{
    bool result = false;
    if (mAccount->id().isValid()) {
        result = QMailStore::instance()->removeAccount(mAccount->id());
        mAccount->setId(QMailAccountId());
    }
    return result;
}

void EmailAccount::test()
{
    QTimer::singleShot(5000, this, SLOT(testConfiguration()));
    //TODO: check for network here when conf manager is integrated
    //with conman
    /*
    QNetworkConfigurationManager networkManager;
    if (networkManager.isOnline()) {
        QTimer::singleShot(5000, this, SLOT(testConfiguration()));
    } else {
        // skip test if not connected to a network
        emit testSkipped();
    }*/
}

void EmailAccount::testConfiguration()
{
    if (mAccount->id().isValid()) {
        mRetrievalAction->retrieveFolderList(mAccount->id(), QMailFolderId(), true);
    } else {
        emit testFailed(InvalidAccount);
    }
}

void EmailAccount::activityChanged(QMailServiceAction::Activity activity)
{
    if (sender() == static_cast<QObject*>(mRetrievalAction)) {
        const QMailServiceAction::Status status(mRetrievalAction->status());

        if (activity == QMailServiceAction::Successful) {
            mTransmitAction->transmitMessages(mAccount->id());
        } else if (activity == QMailServiceAction::Failed) {
            mErrorMessage = status.text;
            mErrorCode = status.errorCode;
            qDebug() << "Testing configuration failed with error " << mErrorMessage << " code: " << mErrorCode;
            emit testFailed(IncomingServer);
        }
    } else if (sender() == static_cast<QObject*>(mTransmitAction)) {
        const QMailServiceAction::Status status(mTransmitAction->status());
        if (activity == QMailServiceAction::Successful) {
            emit testSucceeded();
        } else if (activity == QMailServiceAction::Failed) {
            mErrorMessage = status.text;
            mErrorCode = status.errorCode;
            qDebug() << "Testing configuration failed with error " << mErrorMessage << " code: " << mErrorCode;
            emit testFailed(OutgoingServer);
        }
    }
}

namespace {

    // The only supported types here ('external' type) are: '0':pop3 '1':imap4
    QString externalRecvType(const QString &internal) {
        if (internal == QLatin1String("pop3")) {
            return QLatin1String("0");
        } else if (internal == QLatin1String("imap4")) {
            return QLatin1String("1");
        }
        qWarning() << "Unknown internal receive type:" << internal;
        return QString();
    }

    // Internal type is the stored value
    QString internalRecvType(const QString &external) {
        if (external == QLatin1String("0")) {
            return QLatin1String("pop3");
        } else if (external == QLatin1String("1")) {
            return QLatin1String("imap4");
        }
        qWarning() << "Unknown external receive type:" << external;
        return QString();
    }

    // Equivalent to QMailTransport::EncryptType?
    QString securityType(const QString &securityType) {
        if (securityType == QLatin1String("SSL")) {
            return QLatin1String("1");
        } else if (securityType == QLatin1String("TLS")) {
            return QLatin1String("2");
        }

        if (securityType != QLatin1String("none"))
            qWarning() << "Unknown security type:" << securityType;
        return QLatin1String("0");
    }

    // Equivalent to QMail::SaslMechanism?
    QString authorizationType(const QString &authType) {
        if (authType == QLatin1String("Login")) {
            return QLatin1String("1");
        } else if (authType == QLatin1String("Plain")) {
            return QLatin1String("2");
        } else if (authType == QLatin1String("CRAM-MD5")) {
            return QLatin1String("3");
        }

        if (authType != QLatin1String("none"))
            qWarning() << "Unknown authorization type:" << authType;
        return QLatin1String("0");
    }
}

void EmailAccount::applyPreset()
{
    switch(preset()) {
        case mobilemePreset:
            setRecvType(externalRecvType("imap4"));
            setRecvServer("mail.me.com");
            setRecvPort("993");
            setRecvSecurity(securityType("SSL"));
            setRecvUsername(username());         // username only
            setRecvPassword(password());
            setSendServer("smtp.me.com");
            setSendPort("587");
            setSendSecurity(securityType("SSL"));
            setSendAuth(authorizationType("Login"));
            setSendUsername(username());         // username only
            setSendPassword(password());
            break;
        case gmailPreset:
            setRecvType(externalRecvType("imap4"));
            setRecvServer("imap.gmail.com");
            setRecvPort("993");
            setRecvSecurity(securityType("SSL"));
            setRecvUsername(address());          // full email address
            setRecvPassword(password());
            setSendServer("smtp.gmail.com");
            setSendPort("465");
            setSendSecurity(securityType("SSL"));
            setSendAuth(authorizationType("Login"));
            setSendUsername(address());          // full email address
            setSendPassword(password());
            break;
        case yahooPreset:
            setRecvType(externalRecvType("imap4"));
            setRecvServer("imap.mail.yahoo.com");
            setRecvPort("993");
            setRecvSecurity(securityType("SSL"));
            setRecvUsername(address());          // full email address
            setRecvPassword(password());
            setSendServer("smtp.mail.yahoo.com");
            setSendPort("465");
            setSendSecurity(securityType("SSL"));
            setSendAuth(authorizationType("Login"));
            setSendUsername(address());          // full email address
            setSendPassword(password());
            break;
        case aolPreset:
            setRecvType(externalRecvType("imap4"));
            setRecvServer("imap.aol.com");
            setRecvPort("143");
            setRecvSecurity(securityType("none"));
            setRecvUsername(username());         // username only
            setRecvPassword(password());
            setSendServer("smtp.aol.com");
            setSendPort("587");
            setSendSecurity(securityType("none"));
            setSendAuth(authorizationType("Login"));
            setSendUsername(username());         // username only
            setSendPassword(password());
            break;
        case mslivePreset:
            setRecvType(externalRecvType("pop3"));
            setRecvServer("pop3.live.com");
            setRecvPort("995");
            setRecvSecurity(securityType("SSL"));
            setRecvUsername(address());          // full email address
            setRecvPassword(password());
            setSendServer("smtp.live.com");
            setSendPort("587");
            setSendSecurity(securityType("TLS"));
            setSendAuth(authorizationType("Login"));
            setSendUsername(address());          // full email address
            setSendPassword(password());
            break;
        case noPreset:
            setRecvType(externalRecvType("imap4"));
            setRecvPort("993");
            setRecvSecurity(securityType("SSL"));
            setRecvUsername(username());         // username only
            setRecvPassword(password());
            setSendPort("587");
            setSendSecurity(securityType("SSL"));
            setSendAuth(authorizationType("Login"));
            setSendUsername(username());         // username only
            setSendPassword(password());
            break;
        default:
            break;
    }
}

int EmailAccount::accountId() const
{
    if (mAccount->id().isValid()) {
        return mAccount->id().toULongLong();
    }
    else {
        return -1;
    }
}

void EmailAccount::setAccountId(const int accId)
{
    QMailAccountId accountId(accId);
    if (accountId.isValid()) {
        mAccount = new QMailAccount(accountId);
    }
    else {
        qWarning() << "Invalid account id " << accountId.toULongLong();
    }
}

QString EmailAccount::description() const
{
    return mAccount->name();
}

void EmailAccount::setDescription(QString val)
{
    mAccount->setName(val);
}

bool EmailAccount::enabled() const
{
    return mAccount->status() & QMailAccount::Enabled;
}

void EmailAccount::setEnabled(bool val)
{
    mAccount->setStatus(QMailAccount::Enabled, val);
}

QString EmailAccount::name() const
{
    return mSendCfg->value("username");
}

void EmailAccount::setName(QString val)
{
    mSendCfg->setValue("username", val);
}

QString EmailAccount::address() const
{
    return mSendCfg->value("address");
}

void EmailAccount::setAddress(QString val)
{
    mSendCfg->setValue("address", val);
}

QString EmailAccount::username() const
{
    // read-only property, returns username part of email address
    return address().remove(QRegExp("@.*$"));
}

QString EmailAccount::server() const
{
    // read-only property, returns server part of email address
    return address().remove(QRegExp("^.*@"));
}

QString EmailAccount::password() const
{
    return mPassword;
}

void EmailAccount::setPassword(QString val)
{
    mPassword = val;
}

QString EmailAccount::recvType() const
{
    return externalRecvType(mRecvType);
}

void EmailAccount::setRecvType(QString val)
{
    // prevent bug where recv type gets reset
    // when loading the first time
    QString newRecvType = internalRecvType(val);
    if (newRecvType != mRecvType) {
        mAccountConfig->removeServiceConfiguration(mRecvType);
        mAccountConfig->addServiceConfiguration(newRecvType);
        mRecvType = newRecvType;
        delete mRecvCfg;
        mRecvCfg = new QMailServiceConfiguration(mAccountConfig, mRecvType);
        mRecvCfg->setType(QMailServiceConfiguration::Source);
        mRecvCfg->setVersion(100);
    }
}

QString EmailAccount::recvServer() const
{
    return mRecvCfg->value("server");
}

void EmailAccount::setRecvServer(QString val)
{
    mRecvCfg->setValue("server", val);
}

QString EmailAccount::recvPort() const
{
    return mRecvCfg->value("port");
}

void EmailAccount::setRecvPort(QString val)
{
    mRecvCfg->setValue("port", val);
}

QString EmailAccount::recvSecurity() const
{
    return mRecvCfg->value("encryption");
}

void EmailAccount::setRecvSecurity(QString val)
{
    mRecvCfg->setValue("encryption", val);
}

QString EmailAccount::recvUsername() const
{
    return mRecvCfg->value("username");
}

void EmailAccount::setRecvUsername(QString val)
{
    mRecvCfg->setValue("username", val);
}

QString EmailAccount::recvPassword() const
{
    return Base64::decode(mRecvCfg->value("password"));
}

void EmailAccount::setRecvPassword(QString val)
{
    mRecvCfg->setValue("password", Base64::encode(val));
}

QString EmailAccount::sendServer() const
{
    return mSendCfg->value("server");
}

void EmailAccount::setSendServer(QString val)
{
    mSendCfg->setValue("server", val);
}

QString EmailAccount::sendPort() const
{
    return mSendCfg->value("port");
}

void EmailAccount::setSendPort(QString val)
{
    mSendCfg->setValue("port", val);
}

QString EmailAccount::sendAuth() const
{
    return mSendCfg->value("authentication");
}

void EmailAccount::setSendAuth(QString val)
{
    mSendCfg->setValue("authentication", val);
}

QString EmailAccount::sendSecurity() const
{
    return mSendCfg->value("encryption");
}

void EmailAccount::setSendSecurity(QString val)
{
    mSendCfg->setValue("encryption", val);
}

QString EmailAccount::sendUsername() const
{
    return mSendCfg->value("smtpusername");
}

void EmailAccount::setSendUsername(QString val)
{
    mSendCfg->setValue("smtpusername", val);
}

QString EmailAccount::sendPassword() const
{
    return Base64::decode(mSendCfg->value("smtppassword"));
}

void EmailAccount::setSendPassword(QString val)
{
    mSendCfg->setValue("smtppassword", Base64::encode(val));
}

int EmailAccount::preset() const
{
    return mAccount->customField("preset").toInt();
}

void EmailAccount::setPreset(int val)
{
    mAccount->setCustomField("preset", QString::number(val));
}

QString EmailAccount::errorMessage() const
{
    return mErrorMessage;
}

int EmailAccount::errorCode() const
{
    return mErrorCode;
}

QString EmailAccount::toBase64(const QString &value)
{
    return Base64::encode(value);
}

QString EmailAccount::fromBase64(const QString &value)
{
    return Base64::decode(value);
}

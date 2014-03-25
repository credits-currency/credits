#ifndef PAYMENTSERVERTESTS_H
#define PAYMENTSERVERTESTS_H

#include "../paymentserver.h"

#include <QObject>
#include <QTest>

class PaymentServerTests : public QObject
{
    Q_OBJECT

private slots:
    void paymentServerTests();
};

// Dummy class to receive paymentserver signals.
// If Bitcredit_SendCoinsRecipient was a proper QObject, then we could use
// QSignalSpy... but it's not.
class RecipientCatcher : public QObject
{
    Q_OBJECT

public slots:
    void getRecipient(Bitcredit_SendCoinsRecipient r);

public:
    Bitcredit_SendCoinsRecipient recipient;
};

#endif // PAYMENTSERVERTESTS_H

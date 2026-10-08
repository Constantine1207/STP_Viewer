#pragma once

#include <QLabel>
#include <QStatusBar>

namespace Stp {
inline void addAuthorSignature(QStatusBar* bar)
{
    auto signature=new QLabel(QStringLiteral("Powered by AEME. Feedback: xiaoyang8992@gmail.com"),bar);
    signature->setObjectName("authorSignature");
    signature->setTextFormat(Qt::PlainText);
    signature->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    signature->setTextInteractionFlags(Qt::TextSelectableByMouse);
    signature->setToolTip(signature->text());
    signature->setStyleSheet("font-size:11px;color:#596c80;padding:0px 6px;");
    bar->addPermanentWidget(signature);
}
}

#include "dlgsetpassword.h"
#include "ui_dlgsetpassword.h"
#include "settings.h"



DlgSetPassword::DlgSetPassword(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgSetPassword)
{
    ui->setupUi(this);
    ui->labelPasswordMismatch->hide();
}

DlgSetPassword::~DlgSetPassword()
{
    delete ui;
}


void DlgSetPassword::on_pushButtonClose_clicked()
{
    reject();
}

void DlgSetPassword::on_pushButtonOk_clicked()
{
    // It doubles as the admin login for the embedded API, which anyone on the network
    // can reach, so an empty one is not allowed.
    if (ui->lineEditPassword1->text().isEmpty())
    {
        ui->labelPasswordMismatch->setText(tr("Password can't be empty!"));
        ui->labelPasswordMismatch->show();
    }
    else if (ui->lineEditPassword1->text() != ui->lineEditPassword2->text())
    {
        ui->labelPasswordMismatch->setText(tr("Passwords didn't match!"));
        ui->labelPasswordMismatch->show();
    }
    else
    {
        Settings settings;
        settings.setPassword(ui->lineEditPassword1->text());
        password = ui->lineEditPassword1->text();
        accept();
    }
}

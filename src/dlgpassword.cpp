#include "dlgpassword.h"
#include "ui_dlgpassword.h"
#include "settings.h"

#include <QMessageBox>

DlgPassword::DlgPassword(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgPassword)
{
    ui->setupUi(this);
    ui->label->hide();
}

DlgPassword::~DlgPassword()
{
    delete ui;
}

void DlgPassword::on_pushButtonOk_clicked()
{
    Settings settings;
    if (settings.chkPassword(ui->lineEditPassword->text()))
    {
        ui->label->hide();
        password = ui->lineEditPassword->text();
        accept();
    }
    else
    {
        ui->label->show();
    }
}

void DlgPassword::on_pushButtonCancel_clicked()
{
    reject();
}

void DlgPassword::on_pushButtonReset_clicked()
{
    QMessageBox msgBox;
    msgBox.setWindowTitle(tr("Clear password?"));
    msgBox.setText(tr("Warning, this will erase all secured account and credit card data."));
    msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    // exec() hands back the button pressed, not QDialog::Accepted, so comparing it to
    // Accepted meant the reset never happened.
    if (msgBox.exec() == QMessageBox::Ok)
    {
        Settings settings;
        settings.clearPassword();
        // There is no password left to type in.
        reject();
    }

}

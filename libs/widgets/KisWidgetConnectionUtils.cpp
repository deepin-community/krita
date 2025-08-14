/*
 *  SPDX-FileCopyrightText: 2022 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisWidgetConnectionUtils.h"

#include <QMetaObject>
#include <QMetaProperty>
#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QButtonGroup>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include "kis_debug.h"
#include "kis_spacing_selection_widget.h"
#include "kis_multipliers_double_slider_spinbox.h"
#include "KisAngleSelector.h"
#include "kis_file_name_requester.h"

class ConnectButtonStateHelper : public QObject
{
    Q_OBJECT
public:

    ConnectButtonStateHelper(QAbstractButton *parent)
        : QObject(parent),
          m_button(parent)
    {
    }
public Q_SLOTS:
    void updateState(const CheckBoxState &state) {
        QSignalBlocker b(m_button);
        m_button->setEnabled(state.enabled);
        m_button->setChecked(state.value);

        // TODO: verify if the two properties are equal or the control is disabled
    }

private:
    QAbstractButton *m_button;
};

class ConnectComboBoxStateHelper : public QObject
{
    Q_OBJECT
public:

    ConnectComboBoxStateHelper(QComboBox *parent)
        : QObject(parent),
          m_comboBox(parent)
    {
    }
public Q_SLOTS:
    void updateState(const ComboBoxState &state) {
        QSignalBlocker b(m_comboBox);

        while (m_comboBox->count() > 0) {
            m_comboBox->removeItem(0);
        }

        m_comboBox->addItems(state.items);
        m_comboBox->setCurrentIndex(state.currentIndex);
        m_comboBox->setEnabled(state.enabled);

        // TODO: verify if the two properties are equal or the control is disabled
    }

private:
    QComboBox *m_comboBox;
};


namespace KisWidgetConnectionUtils {

template<typename Button>
void connectButtonLikeControl(Button *button, QObject *source, const char *property)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty prop = meta->property(meta->indexOfProperty(property));

    KIS_SAFE_ASSERT_RECOVER_RETURN(prop.hasNotifySignal());

    QMetaMethod signal = prop.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("bool"));

    const QMetaObject* dstMeta = button->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
        dstMeta->indexOfSlot("setChecked(bool)"));
    QObject::connect(source, signal, button, updateSlot);

    button->setChecked(prop.read(source).toBool());

    if (prop.isWritable()) {
        QObject::connect(button, &Button::toggled,
                         source, [prop, source] (bool value) { prop.write(source, value); });
    }
}

void connectControl(QAbstractButton *button, QObject *source, const char *property)
{
    connectButtonLikeControl(button, source, property);
}

void connectControl(QAction *button, QObject *source, const char *property)
{
    connectButtonLikeControl(button, source, property);
}

void connectControl(QSpinBox *spinBox, QObject *source, const char *property)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty prop = meta->property(meta->indexOfProperty(property));

    KIS_SAFE_ASSERT_RECOVER_RETURN(prop.hasNotifySignal());

    QMetaMethod signal = prop.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("int"));

    const QMetaObject* dstMeta = spinBox->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("setValue(int)"));
    QObject::connect(source, signal, spinBox, updateSlot);

    spinBox->setValue(prop.read(source).toInt());

    if (prop.isWritable()) {
        QObject::connect(spinBox, qOverload<int>(&QSpinBox::valueChanged),
                         source, [prop, source] (int value) { prop.write(source, value); });
    }
}

void connectControl(QDoubleSpinBox *spinBox, QObject *source, const char *property)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty prop = meta->property(meta->indexOfProperty(property));

    KIS_SAFE_ASSERT_RECOVER_RETURN(prop.hasNotifySignal());

    QMetaMethod signal = prop.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("qreal"));

    const QMetaObject* dstMeta = spinBox->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("setValue(qreal)"));
    QObject::connect(source, signal, spinBox, updateSlot);

    spinBox->setValue(prop.read(source).toReal());

    if (prop.isWritable()) {
        QObject::connect(spinBox, qOverload<qreal>(&QDoubleSpinBox::valueChanged),
                         source, [prop, source] (qreal value) { prop.write(source, value); });
    }
}

class ConnectIntSpinBoxStateHelper : public QObject
{
    Q_OBJECT
public:

    ConnectIntSpinBoxStateHelper(QSpinBox *parent)
        : QObject(parent),
        m_spinBox(parent)
    {
    }
public Q_SLOTS:
    void setState(IntSpinBoxState state) {
        QSignalBlocker b(m_spinBox);

        m_spinBox->setEnabled(state.enabled);
        m_spinBox->setRange(state.min, state.max);
        m_spinBox->setValue(state.value);
    }

private:
    QSpinBox *m_spinBox;
};


void connectControlState(QSpinBox *spinBox, QObject *source, const char *readStateProperty, const char *writeProperty)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty readStateProp = meta->property(meta->indexOfProperty(readStateProperty));

    KIS_SAFE_ASSERT_RECOVER_RETURN(readStateProp.hasNotifySignal());

    QMetaMethod signal = readStateProp.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("IntSpinBoxState"));

    ConnectIntSpinBoxStateHelper *helper = new ConnectIntSpinBoxStateHelper(spinBox);

    const QMetaObject* dstMeta = helper->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
        dstMeta->indexOfSlot("setState(IntSpinBoxState)"));
    QObject::connect(source, signal, helper, updateSlot);

    helper->setState(readStateProp.read(source).value<IntSpinBoxState>());

    QMetaProperty writeProp = meta->property(meta->indexOfProperty(writeProperty));
    if (writeProp.isWritable()) {
        QObject::connect(spinBox, qOverload<int>(&QSpinBox::valueChanged),
                         source, [writeProp, source] (int value) { writeProp.write(source, value); });
    }
}

class ConnectDoubleSpinBoxStateHelper : public QObject
{
    Q_OBJECT
public:

    ConnectDoubleSpinBoxStateHelper(QDoubleSpinBox *parent)
        : QObject(parent),
        m_spinBox(parent)
    {
    }
public Q_SLOTS:
    void setState(DoubleSpinBoxState state) {
        QSignalBlocker b(m_spinBox);

        m_spinBox->setEnabled(state.enabled);
        m_spinBox->setRange(state.min, state.max);
        m_spinBox->setValue(state.value);
    }

private:
    QDoubleSpinBox *m_spinBox;
};


void connectControlState(QDoubleSpinBox *spinBox, QObject *source, const char *readStateProperty, const char *writeProperty)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty readStateProp = meta->property(meta->indexOfProperty(readStateProperty));

    KIS_SAFE_ASSERT_RECOVER_RETURN(readStateProp.hasNotifySignal());

    QMetaMethod signal = readStateProp.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("DoubleSpinBoxState"));

    ConnectDoubleSpinBoxStateHelper *helper = new ConnectDoubleSpinBoxStateHelper(spinBox);

    const QMetaObject* dstMeta = helper->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
        dstMeta->indexOfSlot("setState(DoubleSpinBoxState)"));
    QObject::connect(source, signal, helper, updateSlot);

    helper->setState(readStateProp.read(source).value<DoubleSpinBoxState>());

    QMetaProperty writeProp = meta->property(meta->indexOfProperty(writeProperty));
    if (writeProp.isWritable()) {
        QObject::connect(spinBox, qOverload<qreal>(&QDoubleSpinBox::valueChanged),
                         source, [writeProp, source] (qreal value) { writeProp.write(source, value); });
    }
}


void connectControl(KisMultipliersDoubleSliderSpinBox *spinBox, QObject *source, const char *property)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty prop = meta->property(meta->indexOfProperty(property));

    KIS_SAFE_ASSERT_RECOVER_RETURN(prop.hasNotifySignal());

    QMetaMethod signal = prop.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("qreal"));

    const QMetaObject* dstMeta = spinBox->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("setValue(qreal)"));
    QObject::connect(source, signal, spinBox, updateSlot);

    spinBox->setValue(prop.read(source).toReal());

    if (prop.isWritable()) {
        QObject::connect(spinBox, qOverload<qreal>(&KisMultipliersDoubleSliderSpinBox::valueChanged),
                         source, [prop, source] (qreal value) { prop.write(source, value); });
    }
}


class ConnectButtonGroupHelper : public QObject
{
    Q_OBJECT
public:

    ConnectButtonGroupHelper(QButtonGroup *parent)
        : QObject(parent),
          m_buttonGroup(parent)
    {
        QObject::connect(m_buttonGroup, qOverload<QAbstractButton *>(&QButtonGroup::buttonClicked), this, &ConnectButtonGroupHelper::slotButtonClicked);
    }
public Q_SLOTS:
    void updateState(int value) {
        QAbstractButton *btn = m_buttonGroup->button(value);
        KIS_SAFE_ASSERT_RECOVER_RETURN(btn);
        btn->setChecked(true);
    }

    void updateState(ButtonGroupState state) {
        QAbstractButton *btn = m_buttonGroup->button(state.value);
        KIS_SAFE_ASSERT_RECOVER_RETURN(btn);
        btn->setChecked(true);

        Q_FOREACH (QAbstractButton *btn, m_buttonGroup->buttons()) {
            btn->setEnabled(state.enabled);
        }
    }

    void slotButtonClicked(QAbstractButton *btn) {
        int id = m_buttonGroup->id(btn);
        KIS_SAFE_ASSERT_RECOVER_RETURN(id >= 0);

        Q_EMIT idClicked(id);
    }

Q_SIGNALS:
    // this signal was added only in Qt 5.15
    void idClicked(int id);

private:
    QButtonGroup *m_buttonGroup;
};

void connectControl(QButtonGroup *group, QObject *source, const char *property)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty prop = meta->property(meta->indexOfProperty(property));

    KIS_SAFE_ASSERT_RECOVER_RETURN(prop.hasNotifySignal());

    QMetaMethod signal = prop.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("int"));


    ConnectButtonGroupHelper *helper = new ConnectButtonGroupHelper(group);

    const QMetaObject* dstMeta = helper->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("updateState(int)"));
    QObject::connect(source, signal, helper, updateSlot);

    helper->updateState(prop.read(source).toInt());

    if (prop.isWritable()) {
        QObject::connect(helper, &ConnectButtonGroupHelper::idClicked,
                         source, [prop, source] (int value) { prop.write(source, value); });
    }
}

void connectControlState(QButtonGroup *group, QObject *source, const char *readStateProperty, const char *writeProperty)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty readStateProp = meta->property(meta->indexOfProperty(readStateProperty));

    KIS_SAFE_ASSERT_RECOVER_RETURN(readStateProp.hasNotifySignal());

    QMetaMethod signal = readStateProp.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("ButtonGroupState"));

    ConnectButtonGroupHelper *helper = new ConnectButtonGroupHelper(group);

    const QMetaObject* dstMeta = helper->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("updateState(ButtonGroupState)"));
    QObject::connect(source, signal, helper, updateSlot);

    helper->updateState(readStateProp.read(source).value<ButtonGroupState>());

    QMetaProperty writeProp = meta->property(meta->indexOfProperty(writeProperty));
    if (writeProp.isWritable()) {
        QObject::connect(helper, &ConnectButtonGroupHelper::idClicked,
                         source, [writeProp, source] (int value) { writeProp.write(source, value); });
    }
}

void connectControlState(QAbstractButton *button, QObject *source, const char *readStatePropertyName, const char *writePropertyName)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty readStateProp = meta->property(meta->indexOfProperty(readStatePropertyName));

    KIS_SAFE_ASSERT_RECOVER_RETURN(readStateProp.hasNotifySignal());

    QMetaMethod signal = readStateProp.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("CheckBoxState"));

    ConnectButtonStateHelper *helper = new ConnectButtonStateHelper(button);

    const QMetaObject* dstMeta = helper->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("updateState(CheckBoxState)"));
    QObject::connect(source, signal, helper, updateSlot);

    helper->updateState(readStateProp.read(source).value<CheckBoxState>());

    QMetaProperty writeProp = meta->property(meta->indexOfProperty(writePropertyName));
    if (writeProp.isWritable()) {
        button->connect(button, &QAbstractButton::toggled,
                        source, [writeProp, source] (bool value) { writeProp.write(source, value); });
    }
}


void connectControlState(QComboBox *button, QObject *source, const char *readStatePropertyName, const char *writePropertyName)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty readStateProp = meta->property(meta->indexOfProperty(readStatePropertyName));

    KIS_SAFE_ASSERT_RECOVER_RETURN(readStateProp.hasNotifySignal());

    QMetaMethod signal = readStateProp.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("ComboBoxState"));

    ConnectComboBoxStateHelper *helper = new ConnectComboBoxStateHelper(button);

    const QMetaObject* dstMeta = helper->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("updateState(ComboBoxState)"));
    QObject::connect(source, signal, helper, updateSlot);

    helper->updateState(readStateProp.read(source).value<ComboBoxState>());

    QMetaProperty writeProp = meta->property(meta->indexOfProperty(writePropertyName));
    if (writeProp.isWritable()) {
        QObject::connect(button, qOverload<int>(&QComboBox::currentIndexChanged),
                         source, [writeProp, source] (int value) { writeProp.write(source, value); });
    }
}

void connectControl(QComboBox *button, QObject *source, const char *property)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty stateProp = meta->property(meta->indexOfProperty(property));

    KIS_SAFE_ASSERT_RECOVER_RETURN(stateProp.hasNotifySignal());

    QMetaMethod signal = stateProp.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("int"));

    const QMetaObject* dstMeta = button->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("setCurrentIndex(int)"));
    QObject::connect(source, signal, button, updateSlot);

    button->setCurrentIndex(stateProp.read(source).value<int>());

    if (stateProp.isWritable()) {
        QObject::connect(button, qOverload<int>(&QComboBox::currentIndexChanged),
                         source, [stateProp, source] (int value) { stateProp.write(source, value); });
    }
}

class ConnectSpacingWidgetHelper : public QObject
{
    Q_OBJECT
public:

    ConnectSpacingWidgetHelper(KisSpacingSelectionWidget *parent)
        : QObject(parent),
          m_spacingWidget(parent)
    {
        connect(parent, &KisSpacingSelectionWidget::sigSpacingChanged,
                this, &ConnectSpacingWidgetHelper::slotWidgetChanged);
    }
public Q_SLOTS:
    void slotWidgetChanged() {
        Q_EMIT sigWidgetChanged({m_spacingWidget->spacing(), m_spacingWidget->autoSpacingActive(), m_spacingWidget->autoSpacingCoeff()});
    }

    void slotPropertyChanged(SpacingState state) {
        m_spacingWidget->setSpacing(state.useAutoSpacing, state.useAutoSpacing ? state.autoSpacingCoeff : state.spacing);
    }

Q_SIGNALS:
    // this signal was added only in Qt 5.15
    void sigWidgetChanged(SpacingState state);

private:
    KisSpacingSelectionWidget *m_spacingWidget;
};

void connectControl(KisSpacingSelectionWidget *widget, QObject *source, const char *property)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty stateProp = meta->property(meta->indexOfProperty(property));

    KIS_SAFE_ASSERT_RECOVER_RETURN(stateProp.hasNotifySignal());

    QMetaMethod signal = stateProp.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("SpacingState"));

    ConnectSpacingWidgetHelper *helper = new ConnectSpacingWidgetHelper(widget);

    const QMetaObject* dstMeta = helper->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("slotPropertyChanged(SpacingState)"));
    QObject::connect(source, signal, helper, updateSlot);

    helper->slotPropertyChanged(stateProp.read(source).value<SpacingState>());

    if (stateProp.isWritable()) {
        QObject::connect(helper, &ConnectSpacingWidgetHelper::sigWidgetChanged,
                         source, [stateProp, source] (SpacingState value) { stateProp.write(source, QVariant::fromValue(value)); });
    }
}

void connectControl(KisAngleSelector *widget, QObject *source, const char *property)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty prop = meta->property(meta->indexOfProperty(property));

    KIS_SAFE_ASSERT_RECOVER_RETURN(prop.hasNotifySignal());

    QMetaMethod signal = prop.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("qreal"));

    const QMetaObject* dstMeta = widget->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("setAngle(qreal)"));
    QObject::connect(source, signal, widget, updateSlot);

    widget->setAngle(prop.read(source).toReal());

    if (prop.isWritable()) {
        QObject::connect(widget, &KisAngleSelector::angleChanged,
                         source, [prop, source] (qreal value) { prop.write(source, value); });
    }
}

void connectControl(QLineEdit *widget, QObject *source, const char *property)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty prop = meta->property(meta->indexOfProperty(property));

    KIS_SAFE_ASSERT_RECOVER_RETURN(prop.hasNotifySignal());

    QMetaMethod signal = prop.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("QString"));

    const QMetaObject* dstMeta = widget->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("setText(QString)"));
    QObject::connect(source, signal, widget, updateSlot);

    widget->setText(prop.read(source).toString());

    if (prop.isWritable()) {
        QObject::connect(widget, &QLineEdit::textChanged,
                         source, [prop, source] (const QString &value) { prop.write(source, value); });
    }
}

void connectControl(KisFileNameRequester *widget, QObject *source, const char *property)
{
    const QMetaObject* meta = source->metaObject();
    QMetaProperty prop = meta->property(meta->indexOfProperty(property));

    KIS_SAFE_ASSERT_RECOVER_RETURN(prop.hasNotifySignal());

    QMetaMethod signal = prop.notifySignal();

    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterCount() >= 1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(signal.parameterType(0) == QMetaType::type("QString"));
    
    const QMetaObject* dstMeta = widget->metaObject();

    QMetaMethod updateSlot = dstMeta->method(
                dstMeta->indexOfSlot("setFileName(QString)"));
    QObject::connect(source, signal, widget, updateSlot);
    
    widget->setFileName(prop.read(source).toString());

    if (prop.isWritable()) {
        QObject::connect(widget, &KisFileNameRequester::textChanged,
                         source, [prop, source] (const QString &value) {
							 prop.write(source, value); });
    }
}

void connectWidgetVisibleToProperty(QWidget* widget, QObject* source, const char* property)
{
	const QMetaObject* meta = source->metaObject();
    QMetaProperty prop = meta->property(meta->indexOfProperty(property));
	QMetaMethod signal = prop.notifySignal();
	
    const QMetaObject* dstMeta = widget->metaObject();
	
    QMetaMethod updateSlot = dstMeta->method(
        dstMeta->indexOfSlot("setVisible(bool)"));
	
    QObject::connect(source, signal, widget, updateSlot);
    widget->setVisible(prop.read(source).toBool());
}

}

#include <KisWidgetConnectionUtils.moc>

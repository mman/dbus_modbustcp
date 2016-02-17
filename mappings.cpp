#include <QCoreApplication>
#include <QFile>
#include <qmath.h>
#include <QStringList>
#include <velib/qt/ve_qitem.hpp>
#include "dbus_service.h"
#include "dbus_services.h"
#include "mappings.h"
#include "mapping_request.h"
#include "QsLog.h"
#include "ve_qitem_init_monitor.h"

const QString attributesFile = "attributes.csv";
const QString unitIDFile = "unitid2di.csv";
const QString stringType = "string";

Mappings::Mappings(DBusServices *services, QObject *parent) :
	QObject(parent),
	mServices(services)
{
	importCSV(attributesFile);
	importUnitIDMapping(unitIDFile);
}

Mappings::~Mappings()
{
	foreach(DBusModbusData *m, mDBusModbusMap)
		delete m;
}

void Mappings::handleRequest(MappingRequest *request)
{
	DataIterator it(this, request->address(), request->unitId(), request->quantity());
	QList<VeQItem *> pendingItems;
	for (;!it.atEnd(); it.next()) {
		if (request->type() == WriteValues && it.data()->accessRights != Mappings::mb_perm_write) {
			QString errorString = QString("Cannot write to register %1").arg(it.address());
			request->setError(PermissionError, errorString);
			emit requestCompleted(request);
			return;
		}
		pendingItems.append(it.item());
	}
	if (it.error() != NoError) {
		request->setError(it.error(), it.errorString());
		emit requestCompleted(request);
		return;
	}
	addPendingRequest(request, pendingItems);
}

void Mappings::onItemsInitialized()
{
	VeQItemInitMonitor *monitor = static_cast<VeQItemInitMonitor *>(sender());
	monitor->deleteLater();
	MappingRequest *request = mPendingRequests.value(monitor);
	mPendingRequests.remove(monitor);
	onItemsInitialized(request);
}

void Mappings::onItemsInitialized(MappingRequest *request)
{
	switch (request->type()) {
	case ReadValues:
		getValues(request);
		break;
	case WriteValues:
		setValues(request);
		break;
	default:
		Q_ASSERT(false);
		break;
	}
}

quint16 Mappings::getValue(const QVariant &dbusValue, ModbusTypes modbusType, int offset,
						   double scaleFactor)
{
	if (!dbusValue.isValid())
		return 0;
	switch (modbusType) {
	case mb_type_int16:
		return convertFromDbus<qint16>(dbusValue, scaleFactor);
	case mb_type_uint16:
		return convertFromDbus<quint16>(dbusValue, scaleFactor);
	case mb_type_int32:
	{
		quint32 v = convertFromDbus<qint32>(dbusValue, scaleFactor);
		return offset == 0 ? v >> 16 : v & 0xFFFF;
	}
	case mb_type_uint32:
	{
		quint32 v = convertFromDbus<quint32>(dbusValue, scaleFactor);
		return offset == 0 ? v >> 16 : v & 0xFFFF;
	}
	case mb_type_string:
	{
		QByteArray b = dbusValue.toString().toLatin1();
		int index = 2 * offset;
		if (b.size() <= index)
			return 0;
		quint16 v = static_cast<quint16>(b[index] << 8);
		++index;
		if (b.size() <= index)
			return v;
		return v | b[index];
	}
	default:
		return 0;
	}
}

void Mappings::getValues(MappingRequest *request)
{
	DataIterator it(this, request->address(), request->unitId(), request->quantity());
	int j = 0;
	QByteArray &replyData = request->data();
	for (;!it.atEnd(); it.next()) {
		Q_ASSERT(it.error() == NoError);
		VeQItem *item = it.item();
		if (item->getState() == VeQItem::Offline) {
			QLOG_TRACE() << "Value not available" << it.data()->objectPath;
		}
		QVariant dbusValue = item->getValue();
		quint16 value = getValue(dbusValue, it.data()->modbusType, it.offset(),
								 it.data()->scaleFactor);
		replyData[j++] = static_cast<char>(value >> 8);
		replyData[j++] = static_cast<char>(value);
		QLOG_DEBUG() << "Get dbus value" << it.data()->objectPath
					 << "offset" << it.offset() << ':' << dbusValue.toString();
	}
	if (it.error() != NoError) {
		request->setError(it.error(), it.errorString());
		emit requestCompleted(request);
		return;
	}
	emit requestCompleted(request);
}

void Mappings::setValues(MappingRequest *request)
{
	DataIterator it(this, request->address(), request->unitId(), request->quantity());
	int j = 0;
	for (;!it.atEnd(); it.next()) {
		Q_ASSERT(it.error() == NoError);
		VeQItem *item = it.item();
		Q_ASSERT(item->getState() != VeQItem::Requested && item->getState() != VeQItem::Idle);
		quint32 value = 0;
		if (it.registerCount() < it.data()->size || it.offset() > 0) {
			QVariant dbusValue = item->getValue();
			for (int i=0; i<it.data()->size; ++i) {
				quint16 v = getValue(dbusValue, it.data()->modbusType, i, it.data()->scaleFactor);
				value = (value << 16) | v;
			}
		}
		for (int i=it.registerCount(); i>0; --i, j+=2) {
			quint16 v = (static_cast<quint8>(data[j]) << 8) | static_cast<quint8>(data[j+1]);
			int shift = 16 * (it.data()->size - i - it.offset());
			value = (value & ~(0xFFFFu << shift)) | (v << shift);
		}
		QVariant dbusValue;
		switch (it.data()->modbusType) {
		case mb_type_int16:
			dbusValue = convertToDbus(it.data()->dbusType, static_cast<qint16>(value),
									  it.data()->scaleFactor);
			break;
		case mb_type_uint16:
			dbusValue = convertToDbus(it.data()->dbusType, static_cast<quint16>(value),
									  it.data()->scaleFactor);
			break;
		default:
			// Do nothing. dbusValue will remain invalid, which will generate an error below.
			break;
		}
		if (!dbusValue.isValid()) {
			QString errorString = QString("Could not convert value from %1").
					arg(it.data()->objectPath);
			request->setError(ServiceError, errorString);
			emit requestCompleted(request);
			return;
		}
		QLOG_DEBUG() << "Set dbus value" << it.data()->objectPath
					 << "value to" << dbusValue.toString();
		if (item->setValue(dbusValue) != 0) {
			QString errorString = QString("SetValue failed on %1").
					arg(it.data()->objectPath);
			request->setError(ServiceError, errorString);
			emit requestCompleted(request);
			return;
		}
	}
	if (it.error() != NoError) {
		request->setError(it.error(), it.errorString());
		emit requestCompleted(request);
		return;
	}
	emit requestCompleted(request);
}

void Mappings::addPendingRequest(MappingRequest *request, const QList<VeQItem *> &pendingItems)
{
	if (pendingItems.isEmpty()) {
		onItemsInitialized(request);
		return;
	}
	VeQItemInitMonitor *monitor = new VeQItemInitMonitor(this);
	foreach (VeQItem *item, pendingItems)
		monitor->addItem(item);
	mPendingRequests[monitor] = request;
	connect(monitor, SIGNAL(initialized()), this, SLOT(onItemsInitialized()));
	monitor->start();
}

template<class rettype> rettype Mappings::convertFromDbus(const QVariant &value, double scaleFactor)
{
	int variantType = value.userType();

	QMetaType::Type metaType = static_cast<QMetaType::Type>(variantType);
	switch (metaType) {
	case QMetaType::Float:
	case QMetaType::Double:
		return static_cast<rettype>(round(value.toDouble() * scaleFactor));
	case QMetaType::Char:
	case QMetaType::Short:
	case QMetaType::Int:
	case QMetaType::Long:
	case QMetaType::LongLong:
		return static_cast<rettype>(round(value.toInt() * scaleFactor));
	case QMetaType::UChar:
	case QMetaType::UShort:
	case QMetaType::UInt:
	case QMetaType::ULong:
	case QMetaType::ULongLong:
		return static_cast<rettype>(round(value.toUInt() * scaleFactor));
	case QMetaType::Bool:
		return static_cast<rettype>(value.toBool());
	default:
		QLOG_WARN() << "[Mappings] convert from dbus type tries to convert an unsupported type:"
					<< value.type() << "(" << value.typeName() << ")";
		return 0;
	}
}

template<class argtype> QVariant Mappings::convertToDbus(QMetaType::Type dbusType,
														 argtype value, double scaleFactor)
{
	switch (dbusType) {
	case QMetaType::Float:
	case QMetaType::Double:
		return QVariant::fromValue(static_cast<double>(value/scaleFactor));
	case QMetaType::Char:
	case QMetaType::Short:
	case QMetaType::Int:
	case QMetaType::Long:
	case QMetaType::LongLong:
		return QVariant::fromValue(static_cast<int>(round(value/scaleFactor)));
	case QMetaType::UChar:
	case QMetaType::UShort:
	case QMetaType::UInt:
	case QMetaType::ULong:
	case QMetaType::ULongLong:
		return QVariant::fromValue(static_cast<unsigned int>(round(value/scaleFactor)));
	case QMetaType::Bool:
		return QVariant::fromValue(static_cast<int>(value));
	default:
		QLOG_WARN() << "[Mappings] convert to dbus type tries to convert an unsupported type:"
					<< dbusType;
		return QVariant();
	}
}

Mappings::ModbusTypes Mappings::convertModbusType(const QString &typeString)
{
	if (typeString == "int16")
		return mb_type_int16;
	if (typeString == "uint16")
		return mb_type_uint16;
	if (typeString == "int32")
		return mb_type_int32;
	if (typeString == "uint32")
		return mb_type_uint32;
	if (typeString.startsWith(stringType))
		return mb_type_string;
	return mb_type_none;
}

QMetaType::Type Mappings::convertDbusType(const QString &typeString)
{
	if (typeString == "y")
		return QMetaType::UChar;
	else if (typeString == "b")
		return QMetaType::Bool;
	else if (typeString == "n")
		return QMetaType::Short;
	else if (typeString == "q")
		return QMetaType::UShort;
	else if (typeString == "i")
		return QMetaType::Int;
	else if (typeString == "u")
		return QMetaType::UInt;
	else if (typeString == "x")
		return QMetaType::Long;
	else if (typeString == "t")
		return QMetaType::ULong;
	else if (typeString == "d")
		return QMetaType::Double;
	else if (typeString == "s")
		return QMetaType::QString;
	return QMetaType::Void;
}

Mappings::Permissions Mappings::convertPermissions(const QString &permissions)
{
	if (permissions == "R")
		return Mappings::mb_perm_read;
	else if (permissions == "W")
		return Mappings::mb_perm_write;
	return Mappings::mb_perm_none;
}

int Mappings::convertStringSize(const QString &typeString)
{
	if (typeString.size() <= stringType.size() + 2 ||
		typeString[stringType.size()] != '[' ||
		!typeString.endsWith(']')) {
		return 0;
	}
	int offset = stringType.size() + 1;
	int count = typeString.size() - stringType.size() - 2;
	return typeString.mid(offset, count).toInt();
}

void Mappings::importCSV(const QString &filename)
{
	QFile file(QCoreApplication::applicationDirPath() + "/" + filename);
	if (!file.open(QIODevice::ReadOnly)) {
		QLOG_ERROR() << "Can not open file" << filename;
		return;
	}
	QTextStream in(&file);
	while (!in.atEnd()) {
		QString line = in.readLine();
		QStringList values = line.split(",");
		if (values.size() >= 8) {
			ModbusTypes modbusType = convertModbusType(values.at(5));
			if (modbusType != mb_type_none) {
				DBusModbusData * item = new DBusModbusData();
				item->deviceType = DBusService::getDeviceType(values.at(0));
				item->objectPath = values.at(1);
				item->modbusType = convertModbusType(values.at(5));
				item->scaleFactor = values.at(6).toDouble();
				if (item->scaleFactor == 0)
					item->scaleFactor = 1;
				item->dbusType = convertDbusType(values.at(2));
				if (item->dbusType == QMetaType::Void) {
					QLOG_WARN() << "[Mappings] Register" << values.at(4)
								<< ": register has no type";
				}
				item->accessRights = convertPermissions(values.at(7));
				switch (item->modbusType) {
				case mb_type_string:
					item->size = convertStringSize(values.at(5));
					if (item->accessRights == mb_perm_write) {
						item->accessRights = mb_perm_read;
						QLOG_WARN() << "[Mappings] Register" << values.at(4)
									<< ": cannot write string values";
					}
					break;
				case mb_type_int32:
				case mb_type_uint32:
					item->size = 2;
					if (item->accessRights == mb_perm_write) {
						item->accessRights = mb_perm_read;
						QLOG_WARN() << "[Mappings] Register"  << values.at(4)
									<< ": cannot write uin32/int32 values";
					}
					break;
				default:
					item->size = 1;
					break;
				}
				int reg = values.at(4).toInt();
				if (mDBusModbusMap.find(reg) != mDBusModbusMap.end()) {
					QLOG_WARN() << "[Mappings] Register" << reg
								<< "reserved more than once. Check attributes file.";
				}
				mDBusModbusMap.insert(reg, item);
				QLOG_DEBUG() << "[Mappings] Add" << values;
			}
		}
	}
}

void Mappings::importUnitIDMapping(const QString &filename)
{
	QFile file(QCoreApplication::applicationDirPath() + "/" + filename);
	if (!file.open(QIODevice::ReadOnly)) {
		QLOG_ERROR() << "Can not open file" << filename;
		return;
	}
	QTextStream in(&file);
	while (!in.atEnd()) {
		QString line = in.readLine();
		QStringList values = line.split(",");
		if (values.size() >= 2) {
			bool isNumber;
			int unitID = values.at(0).toInt(&isNumber);
			if (isNumber) {
				int deviceInstance = values.at(1).toInt(&isNumber);
				if (isNumber) {
					mUnitIDMap.insert(unitID, deviceInstance);
					QLOG_DEBUG() << "[Mappings] Add" << values;
				}
			}
		}
	}
}

Mappings::DataIterator::DataIterator(const Mappings *mappings, int address, int unitId,
									 int quantity):
	mMappings(mappings),
	mQuantity(quantity),
	mOffset(0),
	mServiceRoot(0),
	mError(NoError)
{
	if (quantity <= 0) {
		mCurrent = mMappings->mDBusModbusMap.end();
		return;
	}

	QHash<int, int>::ConstIterator uIt = mMappings->mUnitIDMap.find(unitId);
	int deviceInstance = 0;
	if (uIt == mMappings->mUnitIDMap.end()) {
		/// If the unit ID is within byte range, and we cannot find it in the mapping, we assume
		/// the unit ID equals  the device instance. This is usefull because device instances
		/// are usually < 256, so we do not have to add all possible device instances to the
		/// mapping.
		if (unitId < 0 || unitId > 255) {
			setError(UnitIdError, QString("Invalid unit ID: %1").arg(unitId));
			return;
		}
		deviceInstance = unitId;
	} else {
		deviceInstance = uIt.value();
	}

	// iterator is value and points to the first item with key >= modbusAddress
	mCurrent = mMappings->mDBusModbusMap.lowerBound(address);
	if (mCurrent == mMappings->mDBusModbusMap.end()) {
		setError(StartAddressError, QString("Modbus address %1 is not registered").arg(address));
		return;
	}
	if (mCurrent.key() != address) {
		if (mCurrent == mMappings->mDBusModbusMap.begin()) {
			setError(StartAddressError, QString("Modbus address %1 is not registered").arg(address));
			return;
		}
		// Note that in a QMap (unlike a QHash) all elements are ordered by key,
		// so --mCurrent will move the iterator to the last element before it. This is
		// the last value with key < modbusAddress.
		--mCurrent;
		Q_ASSERT(mCurrent.key() < address);
		if (mCurrent.key() + mCurrent.value()->size <= address) {
			setError(StartAddressError, QString("Modbus address %1 is not registered").arg(address));
			return;
		}
	}
	mOffset = address - mCurrent.key();

	// Get service from the first modbus address. The service must be the same
	// for the complete address range therefore the service pointer has to be
	// fetched and checked only once
	DBusService *service = mMappings->mServices->getService(mCurrent.value()->deviceType,
															deviceInstance);
	if (service == 0) {
		QString msg = QString("Error finding service with device type %1 at device instance %2").
				arg(mCurrent.value()->deviceType).
				arg(deviceInstance);
		setError(ServiceError, msg);
		return;
	}
	mServiceRoot = service->getServiceRoot();
}

MappingErrors Mappings::DataIterator::error() const
{
	return mError;
}

QString Mappings::DataIterator::errorString() const
{
	return mErrorString;
}

void Mappings::DataIterator::next()
{
	if (mCurrent == mMappings->mDBusModbusMap.end())
		return;
	--mQuantity;
	if (mQuantity == 0) {
		mCurrent = mMappings->mDBusModbusMap.end();
		return;
	}
	++mOffset;
	DBusModbusData *d = mCurrent.value();
	Q_ASSERT(mOffset <= d->size);
	if (mOffset < d->size)
		return;
	int oldAddress = mCurrent.key();
	++mCurrent;
	int newAddress = oldAddress + d->size;
	if (mCurrent == mMappings->mDBusModbusMap.end() || mCurrent.key() != newAddress) {
		setError(AddressError, QString("Modbus address %1 is not registered").arg(newAddress));
		return;
	}
	mOffset = 0;
}

bool Mappings::DataIterator::atEnd() const
{
	return mCurrent == mMappings->mDBusModbusMap.end();
}

const Mappings::DBusModbusData *Mappings::DataIterator::data() const
{
	if (mOffset < 0)
		return 0;
	return mCurrent == mMappings->mDBusModbusMap.end() ? 0 : *mCurrent;
}

VeQItem *Mappings::DataIterator::item() const
{
	if (mCurrent == mMappings->mDBusModbusMap.end())
		return 0;
	return mServiceRoot->itemGetOrCreate(mCurrent.value()->objectPath);
}

int Mappings::DataIterator::offset() const
{
	return mOffset;
}

int Mappings::DataIterator::address() const
{
	return mCurrent == mMappings->mDBusModbusMap.end() ? -1 : mCurrent.key() + mOffset;
}

void Mappings::DataIterator::setError(MappingErrors error, const QString &errorString)
{
	mCurrent = mMappings->mDBusModbusMap.end();
	mError = error;
	mErrorString = errorString;
}

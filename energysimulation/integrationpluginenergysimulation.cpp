/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
*
* Copyright 2013 - 2020, nymea GmbH
* Contact: contact@nymea.io
*
* This file is part of nymea.
* This project including source code and documentation is protected by
* copyright law, and remains the property of nymea GmbH. All rights, including
* reproduction, publication, editing and translation, are reserved. The use of
* this project is subject to the terms of a license agreement to be concluded
* with nymea GmbH in accordance with the terms of use of nymea GmbH, available
* under https://nymea.io/license
*
* GNU Lesser General Public License Usage
* Alternatively, this project may be redistributed and/or modified under the
* terms of the GNU Lesser General Public License as published by the Free
* Software Foundation; version 3. This project is distributed in the hope that
* it will be useful, but WITHOUT ANY WARRANTY; without even the implied
* warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this project. If not, see <https://www.gnu.org/licenses/>.
*
* For any further details and any questions please contact us under
* contact@nymea.io or see our FAQ/Licensing Information on
* https://nymea.io/license/faq
*
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "integrationpluginenergysimulation.h"

#include "plugintimer.h"
#include "plugininfo.h"

#include <QtMath>

#define CIVIL_ZENITH  90.83333


IntegrationPluginEnergySimulation::IntegrationPluginEnergySimulation(QObject *parent): IntegrationPlugin (parent)
{

}

void IntegrationPluginEnergySimulation::discoverThings(ThingDiscoveryInfo *info)
{
    QTimer::singleShot(1000, info, [=]{
        ThingClass thingClass = IntegrationPlugin::thingClass(info->thingClassId());
        for (uint i = 0; i < configValue(energyPluginDiscoveryResultCountParamTypeId).toUInt(); i++) {
            ThingDescriptor descriptor(info->thingClassId(), thingClass.displayName());
            info->addThingDescriptor(descriptor);
        }
        info->finish(Thing::ThingErrorNoError);
    });
}

void IntegrationPluginEnergySimulation::setupThing(ThingSetupInfo *info)
{
    info->finish(Thing::ThingErrorNoError);

    if (!m_timer) {
        m_timer = hardwareManager()->pluginTimerManager()->registerTimer(5);
        connect(m_timer, &PluginTimer::timeout, this, &IntegrationPluginEnergySimulation::updateSimulation);
    }
}


void IntegrationPluginEnergySimulation::thingRemoved(Thing *thing)
{
    Q_UNUSED(thing)
}

void IntegrationPluginEnergySimulation::executeAction(ThingActionInfo *info)
{
    if (info->thing()->thingClassId() == stoveThingClassId) {
        if (info->action().actionTypeId() == stovePowerActionTypeId) {
            info->thing()->setStateValue(stovePowerStateTypeId, info->action().paramValue(stovePowerActionPowerParamTypeId).toBool());
        }
    }
    if (info->thing()->thingClassId() == wallboxThingClassId) {
        if (info->action().actionTypeId() == wallboxPowerActionTypeId) {
            info->thing()->setStateValue(wallboxPowerStateTypeId, info->action().paramValue(wallboxPowerActionPowerParamTypeId).toBool());
        }
        if (info->action().actionTypeId() == wallboxMaxChargingCurrentActionTypeId) {
            info->thing()->setStateValue(wallboxMaxChargingCurrentStateTypeId, info->action().paramValue(wallboxMaxChargingCurrentActionMaxChargingCurrentParamTypeId));
        }
    }
    if (info->thing()->thingClassId() == carThingClassId) {
        if (info->action().actionTypeId() == carPluggedInActionTypeId) {
            if (info->action().paramValue(carPluggedInActionPluggedInParamTypeId).toBool()) {
                // Try to plug the car to the first free wallbox
                foreach (Thing *wallbox, myThings().filterByThingClassId(wallboxThingClassId)) {
                    if (wallbox->property("connectedCarThingId").toUuid().isNull()) {
                        // Found an empty wallbox, plugging it in
                        wallbox->setProperty("connectedCarThingId", info->thing()->id());
                        info->thing()->setStateValue(carPluggedInStateTypeId, true);
                        wallbox->setStateValue(wallboxPluggedInStateTypeId, true);
                        info->finish(Thing::ThingErrorNoError);
                        return;
                    }
                }
                // No wallbox found where we could plug into... Failing action
                info->finish(Thing::ThingErrorHardwareNotAvailable, "No wallbox free wallbox found");
                return;
            } else {
                info->thing()->setStateValue(carPluggedInStateTypeId, false);
                // Unplug from wallbox
                foreach (Thing *wallbox, myThings().filterByThingClassId(wallboxThingClassId)) {
                    if (wallbox->property("connectedCarThingId").toUuid() == info->thing()->id()) {
                        wallbox->setProperty("connectedCarThingId", QUuid());
                        wallbox->setStateValue(wallboxPluggedInStateTypeId, false);
                        break;
                    }
                }
                info->finish(Thing::ThingErrorNoError);
                return;
            }
        } else if (info->action().actionTypeId() == carMinChargingCurrentActionTypeId) {
            info->thing()->setStateValue(carMinChargingCurrentStateTypeId, info->action().paramValue(carMinChargingCurrentActionMinChargingCurrentParamTypeId));
        }
    }
    info->finish(Thing::ThingErrorNoError);
}

void IntegrationPluginEnergySimulation::updateSimulation()
{
    qCDebug(dcEnergy()) << "*******************  Adjusting simulation";
    // Update solar inverters
    QPair<QDateTime, QDateTime> sunriseSunset = calculateSunriseSunset(48, 10, QDateTime::currentDateTime());
    QDateTime sunrise = sunriseSunset.first;
    QDateTime sunset = sunriseSunset.second;
    QDateTime now = QDateTime::currentDateTime();//.addSecs(-60*60*12);
//    qCDebug(dcEnergy()) << "Sunrise:" << sunrise << "Sunset:" << sunset << "Now" << now;
    if (sunrise < now && now < sunset) {
        qlonglong msecsOfLight = sunriseSunset.second.toMSecsSinceEpoch() - sunriseSunset.first.toMSecsSinceEpoch();
        qlonglong currentMSecOfLight = now.toMSecsSinceEpoch() - sunrise.toMSecsSinceEpoch();
        qreal degrees = (currentMSecOfLight * 180 / msecsOfLight) - 90;

        foreach (Thing* inverter, myThings().filterByThingClassId(solarInverterThingClassId)) {
            double currentProduction = qCos(qDegreesToRadians(degrees)) * inverter->setting(solarInverterSettingsMaxCapacityParamTypeId).toDouble();
            qCDebug(dcEnergy()) << "* Inverter" << inverter->name() << "production:" << currentProduction << "W";
            inverter->setStateValue(solarInverterCurrentPowerStateTypeId, -currentProduction);
        }
    } else {
        foreach (Thing* inverter, myThings().filterByThingClassId(solarInverterThingClassId)) {
            inverter->setStateValue(solarInverterCurrentPowerStateTypeId, 0);
        }
    }

    // Update evchargers
    foreach (Thing* evCharger, myThings().filterByThingClassId(wallboxThingClassId)) {
        if (evCharger->stateValue(wallboxPluggedInStateTypeId).toBool() && evCharger->stateValue(wallboxPowerStateTypeId).toBool()) {
            ThingId connectedCarThingId = evCharger->property("connectedCarThingId").toUuid();
            Thing *car = myThings().findById(connectedCarThingId);
            qCDebug(dcEnergy()) << "* Evaluating wallbox:" << evCharger->name() << "Connected car:" << (car ? car->name() : "none");
            if (car && car->stateValue(carBatteryLevelStateTypeId).toInt() < 100) {
                QDateTime lastChargeUpdateTime = car->property("lastChargeUpdateTime").toDateTime();
                if (lastChargeUpdateTime.isNull()) {
                    car->setProperty("lastChargeUpdateTime", QDateTime::currentDateTime());
                    break;
                }
                double maxChargingCurrent = evCharger->stateValue(wallboxMaxChargingCurrentStateTypeId).toDouble();
                double chargingPower = 230 * maxChargingCurrent;
                double chargingTimeHours = 1.0 * lastChargeUpdateTime.msecsTo(QDateTime::currentDateTime()) / 1000 / 60 / 60;
                double chargedWattHours = chargingPower * chargingTimeHours;
                double carCapacity = car->stateValue(carCapacityStateTypeId).toDouble();
                // cWH : cap = x : 100
                double chargedPercentage = chargedWattHours / 1000 * 100 / carCapacity;
                qCDebug(dcEnergy()) << "* #### Car charging info:";
                qCDebug(dcEnergy()) << "* # max charging current:" << maxChargingCurrent;
                qCDebug(dcEnergy()) << "* # time passed since last update:" << chargingTimeHours;
                qCDebug(dcEnergy()) << "* # charged" << chargedWattHours << "Wh," << chargedPercentage << "%";

                if (chargedPercentage >= 1) {
                    car->setProperty("lastChargeUpdateTime", QDateTime::currentDateTime());

                    car->setStateValue(carBatteryLevelStateTypeId, car->stateValue(carBatteryLevelStateTypeId).toInt() + chargedPercentage);
                    car->setStateValue(carBatteryCriticalStateTypeId, car->stateValue(carBatteryLevelStateTypeId).toInt() < 10);
                }
            }
        }
    }

    // Reduce battery level on all unplugged cars
    foreach (Thing *car, myThings().filterByThingClassId(carThingClassId)) {
        if (!car->stateValue(carPluggedInStateTypeId).toBool() && car->stateValue(carBatteryLevelStateTypeId).toInt() > 0) {
            car->setStateValue(carBatteryLevelStateTypeId, car->stateValue(carBatteryLevelStateTypeId).toInt() - 1);
            car->setStateValue(carBatteryCriticalStateTypeId, car->stateValue(carBatteryLevelStateTypeId).toInt() < 10);
        }
    }

    // Update stove
    foreach (Thing *stove, myThings().filterByThingClassId(stoveThingClassId)) {
        if (stove->stateValue(stovePowerStateTypeId).toBool()) {
            int cycle = stove->property("simulationCycle").toInt() % 12;
            double maxPower = stove->setting(stoveSettingsMaxPowerConsumptionParamTypeId).toDouble();
            double currentPower = 0;
            double totalEnergyConsumed = stove->stateValue(stoveTotalEnergyConsumedStateTypeId).toDouble();
            if (cycle < 4) {
                currentPower = maxPower;
                totalEnergyConsumed += (maxPower / 1000) / 60 / 60 * 5;
            }
            stove->setStateValue(stoveCurrentPowerStateTypeId, currentPower);
            stove->setStateValue(stoveTotalEnergyConsumedStateTypeId, totalEnergyConsumed);
            stove->setProperty("simulationCycle", cycle + 1);
        }
    }

    /////////////////////////////////////////////////////
    /// Energy meter
    ////////////////////////////////////////////////////

    // Sum up production from inverters
    QHash<QString, double> totalPhaseProduction = {
        {"A", 0},
        {"B", 0},
        {"C", 0}
    };
    foreach (Thing *inverter, myThings().filterByThingClassId(solarInverterThingClassId)) {
        QString phase = inverter->setting(solarInverterSettingsPhaseParamTypeId).toString();
        double production = inverter->stateValue(solarInverterCurrentPowerStateTypeId).toDouble();
        if (phase == "All") {
            totalPhaseProduction["A"] += production / 3;
            totalPhaseProduction["B"] += production / 3;
            totalPhaseProduction["C"] += production / 3;
        } else {
            totalPhaseProduction[phase] += production;
        }
    }

    // Sum up consumption of all consumers
    QHash<QString, double> totalPhasesConsumption = {
        {"A", 0},
        {"B", 0},
        {"C", 0}
    };
    // Simulate a base consumption of 300W (100 on each phase) + 10W jitter
    totalPhasesConsumption["A"] += 100 + (qrand() % 10);
    totalPhasesConsumption["B"] += 100 + (qrand() % 10);
    totalPhasesConsumption["C"] += 100 + (qrand() % 10);

    // And add simulation devices consumption
    foreach (Thing *consumer, myThings()) {
        // FIXME: energymeter should not inherit smartmeter*
        if (consumer->thingClass().interfaces().contains("smartmeterconsumer") && !consumer->thingClass().interfaces().contains("energymeter")) {
            QString phase = consumer->setting("phase").toString();
            totalPhasesConsumption[phase] += consumer->stateValue("currentPower").toDouble();
        }
    }

    // Add evchargers
    foreach (Thing *evCharger, myThings().filterByThingClassId(wallboxThingClassId)) {
        Thing *connectedCar = myThings().findById(evCharger->property("connectedCarThingId").toUuid());
        if (evCharger->stateValue(wallboxPowerStateTypeId).toBool()
                && evCharger->stateValue(wallboxPluggedInStateTypeId).toBool()
                && connectedCar && connectedCar->stateValue(carBatteryLevelStateTypeId).toInt() < 100) {
            double maxChargingCurrent = evCharger->stateValue(wallboxMaxChargingCurrentStateTypeId).toDouble();
            double currentConsumption = maxChargingCurrent * 230;
            qCDebug(dcEnergy()) << "* Wallbox" << evCharger->name() << "consumes" << currentConsumption << "W";
            QString phase = evCharger->setting(wallboxSettingsPhaseParamTypeId).toString();
            if (phase == "All") {
                totalPhasesConsumption["A"] += currentConsumption / 3;
                totalPhasesConsumption["B"] += currentConsumption / 3;
                totalPhasesConsumption["C"] += currentConsumption / 3;
            } else {
                totalPhasesConsumption[phase] += currentConsumption;
            }
        }
    }


    // Sum up all phases for the total consumption/production (momentary, in Watt)
    double totalProduction = 0;
    foreach (double phaseProduction, totalPhaseProduction) {
        totalProduction += phaseProduction;
    }
    double totalConsumption = 0;
    foreach (double phaseConsumption, totalPhasesConsumption) {
        totalConsumption += phaseConsumption;
    }
    double grandTotal = totalConsumption + totalProduction; // Note: production is negative

    qCDebug(dcEnergy()) << "* Grand total power consumption:" << grandTotal << "W";

    foreach (Thing *smartMeter, myThings().filterByThingClassId(smartMeterThingClassId)) {
        // First set current power consumptions
        qCDebug(dcEnergy()) << "* Updating smart meter:" << smartMeter->name();
        smartMeter->setStateValue(smartMeterCurrentPowerPhaseAStateTypeId, totalPhasesConsumption["A"] + totalPhaseProduction["A"]);
        smartMeter->setStateValue(smartMeterCurrentPowerPhaseBStateTypeId, totalPhasesConsumption["B"] + totalPhaseProduction["B"]);
        smartMeter->setStateValue(smartMeterCurrentPowerPhaseCStateTypeId, totalPhasesConsumption["C"] + totalPhaseProduction["C"]);
        smartMeter->setStateValue(smartMeterCurrentPowerStateTypeId, grandTotal);

        // Add up total consumed/returned
        // Transform current power to kWh for the last 5 secs (simulation interval)
        double consumption = grandTotal / 1000 / 60 / 60 * 5;
        if (grandTotal > 0) {
            double totalEnergyConsumed = smartMeter->stateValue(smartMeterTotalEnergyConsumedStateTypeId).toDouble();
            smartMeter->setStateValue(smartMeterTotalEnergyConsumedStateTypeId, totalEnergyConsumed + consumption);
        } else {
            double totalEnergyReturned = smartMeter->stateValue(smartMeterTotalEnergyProducedStateTypeId).toDouble();
            smartMeter->setStateValue(smartMeterTotalEnergyProducedStateTypeId, totalEnergyReturned - consumption);
        }
    }
}

QPair<QDateTime, QDateTime> IntegrationPluginEnergySimulation::calculateSunriseSunset(qreal latitude, qreal longitude, const QDateTime &dateTime)
{
    int dayOfYear = dateTime.date().dayOfYear();
    int offset = dateTime.offsetFromUtc() / 60 / 60;

    // Convert the longitude to hour value and calculate an approximate time
    qreal longitudeHour = longitude / 15;
    qreal tRise = dayOfYear + ((6 - longitudeHour) / 24);
    qreal tSet = dayOfYear + ((18 - longitudeHour) / 24);

    // Calculate the Sun's mean anomaly
    qreal mRise = (0.9856 * tRise) - 3.289;
    qreal mSet = (0.9856 * tSet) - 3.289;

    // Calculate the Sun's true longitude, and adjust angle to be between 0 and 360
    qreal tmp = mRise + (1.916 * qSin(qDegreesToRadians(mRise))) + (0.020 * qSin(qDegreesToRadians(2 * mRise))) + 282.634;
    qreal lRise = qFloor(tmp + 360) % 360 + (tmp - qFloor(tmp));
    tmp = mSet + (1.916 * qSin(qDegreesToRadians(mSet))) + (0.020 * qSin(qDegreesToRadians(2 * mSet))) + 282.634;
    qreal lSet = qFloor(tmp + 360) % 360 + (tmp - qFloor(tmp));

    // Calculate the Sun's right ascension, and adjust angle to be between 0 and 360
    tmp = qRadiansToDegrees(qAtan(0.91764 * qTan(qDegreesToRadians(1.0 * lRise))));
    qreal raRise = qFloor(tmp + 360) % 360 + (tmp - qFloor(tmp));
    tmp = qRadiansToDegrees(qAtan(0.91764 * qTan(qDegreesToRadians(1.0 * lSet))));
    qreal raSet = qRound(tmp + 360) % 360 + (tmp - qFloor(tmp));

    // Right ascension value needs to be in the same quadrant as L
    qlonglong lQuadrantRise  = qFloor(lRise/90) * 90;
    qlonglong raQuadrantRise = qFloor(raRise/90) * 90;
    raRise = raRise + (lQuadrantRise - raQuadrantRise);

    qlonglong lQuadrantSet  = qFloor(lSet/90) * 90;
    qlonglong raQuadrantSet = qFloor(raSet/90) * 90;
    raSet = raSet + (lQuadrantSet - raQuadrantSet);

    // Right ascension value needs to be converted into hours
    raRise = raRise / 15;
    raSet = raSet / 15;

    // Calculate the Sun's declination
    qreal sinDecRise = 0.39782 * qSin(qDegreesToRadians(1.0 * lRise));
    qreal cosDecRise = qCos(qAsin(sinDecRise));

    qreal sinDecSet = 0.39782 * qSin(qDegreesToRadians(1.0 * lSet));
    qreal cosDecSet = qCos(qAsin(sinDecSet));

    // Calculate the Sun's local hour angle
    qreal cosZenith = qCos(qDegreesToRadians(CIVIL_ZENITH));
    qreal radianLat = qDegreesToRadians(latitude);
    qreal sinLatitude = qSin(radianLat);
    qreal cosLatitude = qCos(radianLat);
    qreal cosHRise = (cosZenith - (sinDecRise * sinLatitude)) / (cosDecRise * cosLatitude);
    qreal cosHSet = (cosZenith - (sinDecSet * sinLatitude)) / (cosDecSet * cosLatitude);

    // Finish calculating H and convert into hours
    qreal hRise = (360 - qRadiansToDegrees(qAcos(cosHRise))) / 15;
    qreal hSet = qRadiansToDegrees(qAcos(cosHSet)) / 15;

    // Calculate local mean time of rising/setting
    tRise = hRise + raRise - (0.06571 * tRise) - 6.622;
    tSet = hSet + raSet - (0.06571 * tSet) - 6.622;

    // Adjust back to UTC, and keep the time between 0 and 24
    tmp = tRise - longitudeHour;
    qreal utRise = qFloor(tmp + 24) % 24 + (tmp - qFloor(tmp));
    tmp = tSet - longitudeHour;
    qreal utSet = qFloor(tmp + 24) % 24 + (tmp - qFloor(tmp));

    // Adjust again to localtime
    tmp = utRise + offset;
    qreal localtRise = qFloor(tmp + 24) % 24 + (tmp - qFloor(tmp));
    tmp = utSet + offset;
    qreal localtSet = qFloor(tmp + 24) % 24 + (tmp - qFloor(tmp));

    // Conversion
    int hourRise = qFloor(localtRise);
    int minuteRise = qFloor((localtRise - qFloor(localtRise)) * 60);
    int hourSet = qFloor(localtSet);
    int minuteSet = qFloor((localtSet - qFloor(localtSet)) * 60);

    QDateTime sunrise(dateTime.date(), QTime(hourRise, minuteRise));
    QDateTime sunset(dateTime.date(), QTime(hourSet, minuteSet));

    return qMakePair<QDateTime, QDateTime>(sunrise, sunset);
}

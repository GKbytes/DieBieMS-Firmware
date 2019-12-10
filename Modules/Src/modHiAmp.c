#include "modHiAmp.h"

modPowerElectronicsPackStateTypedef *modHiAmpPackStateHandle;
modConfigGeneralConfigStructTypedef *modHiAmpGeneralConfigHandle;

uint32_t modHiAmpShieldPresenceDetectLastTick;
uint32_t modHiAmpShieldSamplingLastTick;
uint32_t modHiAmpShieldRelayTimeoutLastTick;
uint32_t modHiAmpShieldRelayStartPrechargeTimeStamp;

relayControllerStateTypeDef modHiAmpShieldRelayControllerRelayEnabledState;
relayControllerStateTypeDef modHiAmpShieldRelayControllerRelayEnabledLastState;

bool dischargeHCEnable;
bool modHiAmpShieldRelayControllerRelayEnabledDesiredLastState;
bool modHiAmpShieldPrePreChargeBulkCapChargeDetected;

bool HVEnableDischarge = false;
bool HVEnablePreCharge = false;
bool HVEnableLowSide   = false;
bool HVEnableCharge    = false;

uint8_t newFanSpeed = 0;

float tempVoltage;

void modHiAmpInit(modPowerElectronicsPackStateTypedef* packStateHandle, modConfigGeneralConfigStructTypedef *generalConfigPointer){
	modHiAmpPackStateHandle     = packStateHandle;																		// Store pack state pointer.
	modHiAmpGeneralConfigHandle = generalConfigPointer;
	
	// Init the communication bus
	driverHWI2C1Init();
	
	// Check whether the slave sensors are present
	modHiAmpPackStateHandle->slaveShieldPresenceOCPotmeter = false;
	modHiAmpPackStateHandle->slaveShieldPresenceFanDriver  = false;
	modHiAmpPackStateHandle->slaveShieldPresenceAuxADC     = false;
	modHiAmpPackStateHandle->slaveShieldPresenceADSADC     = false;
	modHiAmpPackStateHandle->slaveShieldPresenceMasterISL  = false;
	modHiAmpPackStateHandle->slaveShieldPresenceMainISL    = false;
	modHiAmpPackStateHandle->hiAmpShieldPresent            = modHiAmpShieldPresentCheck();
	
	// Initialisation variables
	modHiAmpShieldResetVariables();																										// Reset the hiAmp shield variables
	modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_OFF;
	modHiAmpShieldRelayControllerRelayEnabledLastState = RELAY_CONTROLLER_INIT;
	modHiAmpShieldRelayControllerRelayEnabledDesiredLastState = false;
	modHiAmpShieldPrePreChargeBulkCapChargeDetected = false;
	modHiAmpShieldRelayStartPrechargeTimeStamp = HAL_GetTick();
	
	// Initialise slave board
	if(modHiAmpPackStateHandle->hiAmpShieldPresent){
		driverSWDCDCInit(modHiAmpPackStateHandle,modHiAmpGeneralConfigHandle);					// Init the DCDC converter enviroment
		driverSWDCDCSetEnabledState(true);																							// Enable the converter
		driverSWPCAL6416Init(0x00,0x56,0x00,0x56,0x00,0x56);														// Init the IO Extender
		driverSWADC128D818Init();																												// Init the NTC ADC
		driverSWSHT21Init();																														// Init the Temperature / humidity sensor
		
		if(modHiAmpPackStateHandle->slaveShieldPresenceMainISL){
			modHiAmpShieldMainPathMonitorInit();
		}
		
		if(modHiAmpPackStateHandle->slaveShieldPresenceFanDriver){
			driverSWEMC2305Init(I2CADDRFANDriver,100);																		// Init the FANDriver with addres and minimal duty cycle
			modHiAmpShieldSetFANSpeedAll(0);																							// Disable all FANs
		}
		
		if(modHiAmpPackStateHandle->slaveShieldPresenceAuxADC){
			driverSWMCP3221Init();                                                        // Init the aux ADC
		}
		
		if(modHiAmpPackStateHandle->slaveShieldPresenceADSADC){
			driverSWADS1015Init();
		}else{
			driverSWADS1015ResetValues();
		}
		
		if(modHiAmpPackStateHandle->slaveShieldPresenceOCPotmeter){
			driverSWAD5245Init(I2CADDROCDPOTMETERDriver,modHiAmpGeneralConfigHandle->HCLoadCurrentPotValue);
		}
		
	}else{
	  modHiAmpShieldResetSensors();
	}
}

void modHiAmpTask(void) {
	if(modDelayTick1ms(&modHiAmpShieldPresenceDetectLastTick,5000)){
		modHiAmpPackStateHandle->hiAmpShieldPresent = modHiAmpShieldPresentCheck();
		
		modHiAmpShieldSetFANSpeedAll(newFanSpeed);
		modHiAmpShieldSetOCDPotvalue(modHiAmpGeneralConfigHandle->HCLoadCurrentPotValue);
	}
	
	if(modDelayTick1ms(&modHiAmpShieldSamplingLastTick,100)){
		if(modHiAmpPackStateHandle->hiAmpShieldPresent){
			// Determin whether discharge should be allowed		
			if(modHiAmpGeneralConfigHandle->togglePowerModeDirectHCDelay || modHiAmpGeneralConfigHandle->pulseToggleButton){
				dischargeHCEnable = modHiAmpPackStateHandle->auxDCDCOutputOK && modHiAmpPackStateHandle->disChargeDesired && modHiAmpPackStateHandle->disChargeHCAllowed && modPowerElectronicsHCSafetyCANAndPowerButtonCheck();
			}else{
				dischargeHCEnable = modHiAmpPackStateHandle->auxDCDCOutputOK && modHiAmpPackStateHandle->disChargeDesired && modHiAmpPackStateHandle->disChargeHCAllowed && modHiAmpPackStateHandle->powerButtonActuated && modPowerElectronicsHCSafetyCANAndPowerButtonCheck();
			}

			// Update inputs
			if(modHiAmpPackStateHandle->slaveShieldPresenceFanDriver){
				modHiAmpPackStateHandle->FANStatus = driverEMC2305GetFANStatus(I2CADDRFANDriver);
			}else{
				driverSWEMC2305FanStatusTypeDef emptyState = {{0,0,0,0},false,false,false};
				modHiAmpPackStateHandle->FANStatus = emptyState;
			}
			
			modHiAmpPackStateHandle->auxDCDCEnabled       = driverSWDCDCGetEnabledState();
			modHiAmpPackStateHandle->auxVoltage           = driverSWDCDCGetAuxVoltage();
			modHiAmpPackStateHandle->auxCurrent           = driverSWDCDCGetAuxCurrent();
			modHiAmpPackStateHandle->auxPower             = modHiAmpPackStateHandle->auxVoltage * modHiAmpPackStateHandle->auxCurrent;
			modHiAmpPackStateHandle->auxDCDCOutputOK      = driverSWDCDCCheckVoltage(modHiAmpPackStateHandle->auxVoltage,modHiAmpGeneralConfigHandle->DCDCTargetVoltage,0.1f);
			modHiAmpPackStateHandle->hiCurrentLoadVoltage = modHiAmpShieldShuntMonitorGetVoltage();
			modHiAmpPackStateHandle->hiCurrentLoadCurrent = modHiAmpShieldShuntMonitorGetCurrent();
			modHiAmpPackStateHandle->hiCurrentLoadPower   = modHiAmpPackStateHandle->hiCurrentLoadVoltage * modHiAmpPackStateHandle->hiCurrentLoadCurrent;
			modHiAmpPackStateHandle->hiCurrentLoadState   = modHiAmpShieldRelayControllerRelayEnabledState;
			modHiAmpShieldTemperatureHumidityMeasureTask();
			
			// Update outputs
			modHiAmpShieldRelayControllerPassSampledInput(dischargeHCEnable,modHiAmpPackStateHandle->hiCurrentLoadVoltage,modHiAmpPackStateHandle->packVoltage);
			
			// Update HV Ouputs
			modHiAmpShieldHVSSRTask();
		}else{
			modHiAmpShieldResetSensors();
	  }
	}
	
	driverSWADS1015SampleTask(modHiAmpPackStateHandle->slaveShieldPresenceADSADC);
	driverSWDCDCEnableTask();
	modHiAmpShieldRelayControllerTask();
}

bool modHiAmpShieldPresentCheck(void) {
	uint8_t I2CWrite = 0;
	uint8_t PresenceDetect = 0;
	
	PresenceDetect |= driverHWI2C1Write(I2CADDRISLAux     ,false,&I2CWrite,0);   // ISL Aux DCDC
	PresenceDetect |= driverHWI2C1Write(I2CADDRIOExt      ,false,&I2CWrite,0);   // IO Ext
	PresenceDetect |= driverHWI2C1Write(I2CADDRADC8       ,false,&I2CWrite,0);   // NTC ADC
	
	modHiAmpPackStateHandle->slaveShieldPresenceMasterISL  = (driverHWI2C2Write(I2CADDRISLMaster   ,false,&I2CWrite,0) == HAL_OK) ? true : false;	
	modHiAmpPackStateHandle->slaveShieldPresenceMainISL    = (driverHWI2C1Write(I2CADDRISLMain     ,false,&I2CWrite,0) == HAL_OK) ? true : false;
	modHiAmpPackStateHandle->slaveShieldPresenceFanDriver  = (driverHWI2C1Write(I2CADDRFANDriver   ,false,&I2CWrite,0) == HAL_OK) ? true : false;
	modHiAmpPackStateHandle->slaveShieldPresenceAuxADC     = (driverHWI2C1Write(I2CADDRADC8        ,false,&I2CWrite,0) == HAL_OK) ? true : false;
	modHiAmpPackStateHandle->slaveShieldPresenceADSADC     = (driverHWI2C1Write(I2CADS1015         ,false,&I2CWrite,0) == HAL_OK) ? true : false;
	modHiAmpPackStateHandle->slaveShieldPresenceOCPotmeter = (driverHWI2C1Write(I2CADDROCDPOTMETER ,false,&I2CWrite,0) == HAL_OK) ? true : false;
	
	if(PresenceDetect == HAL_OK)
		return true;
	else
		return false;
}

uint16_t modHiAmpShieldScanI2CDevices(void) {
	uint8_t I2CWrite = 0;
	uint16_t PresenceMask = 0;
	
	PresenceMask |= (driverHWI2C1Write(I2CADDRISLMain    ,false,&I2CWrite,0) == HAL_OK) ? (1 << 0) : false; // ISL Main
	PresenceMask |= (driverHWI2C1Write(I2CADDRISLAux     ,false,&I2CWrite,0) == HAL_OK) ? (1 << 1) : false; // ISL Aux
	PresenceMask |= (driverHWI2C1Write(I2CADDRSHT        ,false,&I2CWrite,0) == HAL_OK) ? (1 << 2) : false; // SHT
	PresenceMask |= (driverHWI2C1Write(I2CADDRIOExt      ,false,&I2CWrite,0) == HAL_OK) ? (1 << 3) : false; // IO Ext
	PresenceMask |= (driverHWI2C1Write(I2CADDRADC8       ,false,&I2CWrite,0) == HAL_OK) ? (1 << 4) : false; // NTC ADC 8 Channel water/temp
	PresenceMask |= (driverHWI2C1Write(I2CADDRADC1       ,false,&I2CWrite,0) == HAL_OK) ? (1 << 5) : false; // NTC ADC	1 Channel strutconn temp
	PresenceMask |= (driverHWI2C1Write(I2CADDRFANDriver  ,false,&I2CWrite,0) == HAL_OK) ? (1 << 6) : false; // FAN Driver
	PresenceMask |= (driverHWI2C1Write(I2CADS1015        ,false,&I2CWrite,0) == HAL_OK) ? (1 << 7) : false; // HV voltage measure ADC	 temp
	PresenceMask |= (driverHWI2C1Write(I2CADDROCDPOTMETER,false,&I2CWrite,0) == HAL_OK) ? (1 << 8) : false; // Potmeter
	PresenceMask |= (driverHWI2C1Write(I2CADDREEPROM     ,false,&I2CWrite,0) == HAL_OK) ? (1 << 9) : false; // EEPROM	
	
  return PresenceMask;
}

void modHiAmpShieldResetVariables(void) {
	modHiAmpPackStateHandle->aux0EnableDesired            = false;
	modHiAmpPackStateHandle->aux0Enabled                  = false;
	modHiAmpPackStateHandle->aux1EnableDesired            = false;
	modHiAmpPackStateHandle->aux1Enabled                  = false;
	modHiAmpPackStateHandle->auxDCDCEnabled               = false;
	modHiAmpPackStateHandle->FANSpeedDutyDesired          = 0;
	modHiAmpPackStateHandle->FANStatus.FANEnabled         = false;
	
  modHiAmpShieldResetSensors();
}

void modHiAmpShieldMainPathMonitorInit(void) {
	if(modHiAmpPackStateHandle->slaveShieldPresenceMainISL) {
		driverSWISL28022InitStruct ISLInitStruct;					 																								// Init the bus voltage and current monitor. (MAIN)
		ISLInitStruct.ADCSetting = ADC_128_64010US;
		ISLInitStruct.busVoltageRange = BRNG_60V_1;
		ISLInitStruct.currentShuntGain = PGA_4_160MV;
		ISLInitStruct.Mode = MODE_SHUNTANDBUS_CONTINIOUS;
		driverSWISL28022Init(ISL28022_SHIELD_MAIN_ADDRES,ISL28022_SHIELD_MAIN_BUS,ISLInitStruct);
	}
	
	if(modHiAmpPackStateHandle->slaveShieldPresenceADSADC) {
	  // Init the ADS
	}
}

float modHiAmpShieldShuntMonitorGetVoltage(void) {
  float measuredVoltage = 0.0f;
	
	switch(modHiAmpGeneralConfigHandle->HCLoadVoltageDataSource) {
	  case sourceLoadHCVoltageNone:
			measuredVoltage = 0.0f;
		  break;
	  case sourceLoadHCVoltageISL28022_2_0X40_LVBatteryIn:
			if(modHiAmpPackStateHandle->slaveShieldPresenceMasterISL) {
				driverSWISL28022GetBusVoltage(ISL28022_MASTER_ADDRES,ISL28022_MASTER_BUS,&measuredVoltage,0.004f);
			}
		  break;
	  case sourceLoadHCVoltageISL28022_1_0X44_LVLoadOutput:
			if(modHiAmpPackStateHandle->slaveShieldPresenceMainISL) {
				driverSWISL28022GetBusVoltage(ISL28022_SHIELD_MAIN_ADDRES,ISL28022_SHIELD_MAIN_BUS,&measuredVoltage,0.004f);
			}
		  break;
	  case sourceLoadHCVoltageISL28022_1_0X45_DCDC:
			if(modHiAmpPackStateHandle->hiAmpShieldPresent) {
			  measuredVoltage = driverSWDCDCGetAuxVoltage();
			}
		  break;
	  case sourceLoadHCVoltageADS1015_AN01_HVBatteryIn:
			if(modHiAmpPackStateHandle->hiAmpShieldPresent) {
				measuredVoltage = driverSWADS1015GetVoltage(ADS1015P0N1,0.00527083333f);
			}
		  break;
	  case sourceLoadHCVoltageADS1015_AN23_HVLoadOut:
			if(modHiAmpPackStateHandle->hiAmpShieldPresent) {
				measuredVoltage = driverSWADS1015GetVoltage(ADS1015P2N3,0.00527083333f);
			}
		  break;
	  case sourceLoadHCVoltageSumOfIndividualCellVoltages:
			measuredVoltage = modHiAmpGeneralConfigHandle->noOfCellsSeries*modHiAmpPackStateHandle->cellVoltageAverage;
		  break;
	  case sourceLoadHCVoltageCANDieBieShunt:
			// Get data from CAN shunt
		  break;
	  case sourceLoadHCVoltageCANIsabellenhutte:
		  // Get data from CAN shunt
		  break;
		default:
		  break;
	}
	
	return measuredVoltage;
}

float modHiAmpShieldShuntMonitorGetCurrent(void) {
  float measuredCurrent = 0.0f;
	
  switch(modHiAmpGeneralConfigHandle->HCLoadCurrentDataSource) {
	  case sourceLoadHCCurrentNone:
			measuredCurrent = 0.0f;
			break;
	  case sourceLoadHCCurrentISL28022_2_0X40_LVLCShunt:
			if(modHiAmpPackStateHandle->slaveShieldPresenceMasterISL) {
				driverSWISL28022GetBusCurrent(ISL28022_MASTER_ADDRES,ISL28022_MASTER_BUS,&measuredCurrent,modHiAmpGeneralConfigHandle->shuntLCOffset,modHiAmpGeneralConfigHandle->shuntLCFactor);
			}
			break;
	  case sourceLoadHCCurrentISL28022_1_0X44_LVHCShunt:
			if(modHiAmpPackStateHandle->slaveShieldPresenceMainISL) {
				driverSWISL28022GetBusCurrent(ISL28022_SHIELD_MAIN_ADDRES,ISL28022_SHIELD_MAIN_BUS,&measuredCurrent,modHiAmpGeneralConfigHandle->shuntHCOffset,modHiAmpGeneralConfigHandle->shuntHCFactor);
			}
			break;
	  case sourceLoadHCCurrentISL28022_1_0X45_DCDCShunt:
			measuredCurrent = driverSWDCDCGetAuxCurrent();
			break;
	  case sourceLoadHCCurrentCANDieBieShunt:
			// Get current from CAN shunt
			break;
	  case sourceLoadHCCurrentCANIsabellenhutte:
			// Get current from CAN shunt
			break;
		default:
			break;
	}

	return measuredCurrent;
}


void modHiAmpShieldSetOCDPotvalue(uint8_t newValue) {
	if(modHiAmpPackStateHandle->slaveShieldPresenceOCPotmeter){
		driverSWAD5245SetPotmeterValue(I2CADDROCDPOTMETERDriver,newValue);
	}
}

void modHiAmpShieldSetFANSpeedAll(uint8_t newFANSpeed) {
	modHiAmpPackStateHandle->FANSpeedDutyDesired = newFANSpeed;
	if(modHiAmpPackStateHandle->slaveShieldPresenceFanDriver){
		driverSWEMC2305SetFANDutyAll(I2CADDRFANDriver,newFANSpeed);
	}
}

void modHiAmpShieldRelayControllerPassSampledInput(uint8_t relayStateDesired, float mainBusVoltage, float batteryVoltage) {
	if(modHiAmpShieldRelayControllerRelayEnabledDesiredLastState != relayStateDesired) {
		if(relayStateDesired && modHiAmpGeneralConfigHandle->HCUseRelay) {
			if(modHiAmpGeneralConfigHandle->HCUsePrecharge) {
				modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_PRECHARGING;
				modHiAmpShieldPrePreChargeBulkCapChargeDetected = (mainBusVoltage > (batteryVoltage*PREPRECHARGE_LOADDETECT)) ? true : false;
			}else{
				modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_ENABLED;
			}
		}else{
			modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_OFF;
		}
		modHiAmpShieldRelayControllerRelayEnabledDesiredLastState = relayStateDesired;
	}
	
	// process new samples and go to precharged state if ok
	if(modHiAmpShieldRelayControllerRelayEnabledState == RELAY_CONTROLLER_PRECHARGING){
		if(mainBusVoltage >= batteryVoltage*PRECHARGE_PERCENTAGE_HC)
			modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_PRECHARGED;
	}
	
	// process new samples and go to timeout state if voltage drops
	if(modHiAmpShieldRelayControllerRelayEnabledState == RELAY_CONTROLLER_ENABLED){
		if((mainBusVoltage <= batteryVoltage*PRECHARGE_PERCENTAGE_HC) && modHiAmpGeneralConfigHandle->HCUsePrecharge)
			modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_TIMOUT;
	}
}

void modHiAmpShieldRelayControllerTask(void) {
	if(modHiAmpShieldRelayControllerRelayEnabledState != modHiAmpShieldRelayControllerRelayEnabledLastState){		// Only on change
		switch(modHiAmpShieldRelayControllerRelayEnabledState) {
			case RELAY_CONTROLLER_INIT:
				modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_OFF;
			  //modHiAmpPackStateHandle->hiCurrentLoadPreChargeDuration = 0;
			case RELAY_CONTROLLER_OFF:
				modHiAmpShieldRelayControllerSetRelayOutputState(false,false);
			  modHiAmpPackStateHandle->hiLoadEnabled = false;
			  modHiAmpPackStateHandle->hiLoadPreChargeEnabled = false;
			  modHiAmpPackStateHandle->hiLoadPreChargeError = false;
			  //modHiAmpPackStateHandle->hiCurrentLoadPreChargeDuration = 0;
			  modHiAmpPackStateHandle->hiCurrentLoadDetected = false;
				break;
			case RELAY_CONTROLLER_PRECHARGING:
			  modHiAmpShieldRelayControllerSetRelayOutputState(false,true);
			  modHiAmpPackStateHandle->hiLoadEnabled = false;
			  modHiAmpPackStateHandle->hiLoadPreChargeEnabled = true;
			  modHiAmpPackStateHandle->hiLoadPreChargeError = false;
			  modHiAmpShieldRelayTimeoutLastTick = HAL_GetTick();
			  modHiAmpShieldRelayStartPrechargeTimeStamp = HAL_GetTick();
			  //modHiAmpPackStateHandle->hiCurrentLoadPreChargeDuration = 0;
			  modHiAmpPackStateHandle->hiCurrentLoadDetected = false;
			  break;
			case RELAY_CONTROLLER_PRECHARGED:
				modHiAmpShieldRelayControllerSetRelayOutputState(true,true);
			  modHiAmpPackStateHandle->hiLoadEnabled = true;
			  modHiAmpPackStateHandle->hiLoadPreChargeEnabled = true;
			  modHiAmpPackStateHandle->hiLoadPreChargeError = false;
			  modHiAmpShieldRelayTimeoutLastTick = HAL_GetTick();
			  modHiAmpPackStateHandle->hiCurrentLoadPreChargeDuration = HAL_GetTick() - modHiAmpShieldRelayStartPrechargeTimeStamp;
			  
			  if(modHiAmpPackStateHandle->hiCurrentLoadPreChargeDuration >= modHiAmpGeneralConfigHandle->HCLoadDetectThreshold) {
					modHiAmpPackStateHandle->hiCurrentLoadDetected = true;
				}else{
					modHiAmpPackStateHandle->hiCurrentLoadDetected = modHiAmpShieldPrePreChargeBulkCapChargeDetected;
				}
				
				break;
			case RELAY_CONTROLLER_TIMOUT:
			case RELAY_CONTROLLER_NOLOAD:				
        modHiAmpShieldRelayControllerSetRelayOutputState(false,false);
			  modHiAmpPackStateHandle->hiLoadEnabled = false;
			  modHiAmpPackStateHandle->hiLoadPreChargeEnabled = false;
			  modHiAmpPackStateHandle->hiLoadPreChargeError = true;
			  modHiAmpShieldRelayTimeoutLastTick = HAL_GetTick();
			  //modHiAmpPackStateHandle->hiCurrentLoadPreChargeDuration = 0;
			  modHiAmpPackStateHandle->hiCurrentLoadDetected = false;
				break;
			case RELAY_CONTROLLER_ENABLED:
				modHiAmpShieldRelayControllerSetRelayOutputState(true,false);
			  modHiAmpPackStateHandle->hiLoadEnabled = true;
			  modHiAmpPackStateHandle->hiLoadPreChargeEnabled = false;
			  modHiAmpPackStateHandle->hiLoadPreChargeError = false;
				break;
			default:
				modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_OFF;
			  //modHiAmpPackStateHandle->hiCurrentLoadPreChargeDuration = 0;
			  modHiAmpPackStateHandle->hiCurrentLoadDetected = false;
				break;
		}
		modHiAmpShieldRelayControllerRelayEnabledLastState = modHiAmpShieldRelayControllerRelayEnabledState;
	}
	
	if(modHiAmpShieldRelayControllerRelayEnabledState == RELAY_CONTROLLER_PRECHARGING){
		// check delay and go to RELAY_CONTROLLER_TIMOUT if triggered
		if(modDelayTick1ms(&modHiAmpShieldRelayTimeoutLastTick,modHiAmpGeneralConfigHandle->timeoutHCPreCharge))
			modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_TIMOUT;
	}
	
	if(modHiAmpShieldRelayControllerRelayEnabledState == RELAY_CONTROLLER_PRECHARGED){
		// wait for main relay to enable and then disable pre charge
		if(modDelayTick1ms(&modHiAmpShieldRelayTimeoutLastTick,modHiAmpGeneralConfigHandle->timeoutHCRelayOverlap)) {
			if(modHiAmpPackStateHandle->hiCurrentLoadDetected || !modHiAmpGeneralConfigHandle->HCUseLoadDetect)
			  modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_ENABLED;
			else
				modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_NOLOAD;
		}
	}	
	
	if((modHiAmpShieldRelayControllerRelayEnabledState == RELAY_CONTROLLER_TIMOUT) || (modHiAmpShieldRelayControllerRelayEnabledState == RELAY_CONTROLLER_NOLOAD)){
		// check delay and go to RELAY_CONTROLLER_PRECHARGING if triggered
		if(modDelayTick1ms(&modHiAmpShieldRelayTimeoutLastTick,modHiAmpGeneralConfigHandle->timeoutHCPreChargeRetryInterval) && modHiAmpGeneralConfigHandle->timeoutHCPreChargeRetryInterval){
			if(modHiAmpGeneralConfigHandle->HCUsePrecharge)
				modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_PRECHARGING;
			else
				modHiAmpShieldRelayControllerRelayEnabledState = RELAY_CONTROLLER_ENABLED;
		}
	}
}

void modHiAmpShieldRelayControllerSetRelayOutputState(bool newStateRelay, bool newStatePreCharge) {
	driverSWPCAL6416SetOutput(0,7,newStatePreCharge,false);																											// Set new state precharge relay
	driverSWPCAL6416SetOutput(0,6,newStateRelay,false);																												  // Set new state relay hold
	driverSWPCAL6416SetOutput(0,5,newStateRelay,true);																													// Set new state relay main and write to chip
}

void  modHiAmpShieldTemperatureHumidityMeasureTask(void) {
	// Static variable for SHT measure timer
	static uint32_t measureSHTStartLastTick          = 0;
	static driverSWSHT21MeasureType lastMeasuredType = TEMP;
	
	// Measure pack NTC's	(external)
	modHiAmpPackStateHandle->temperatures[4] = driverSWADC128D818GetTemperature(modHiAmpGeneralConfigHandle->NTC25DegResistance[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCTopResistor[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCBetaFactor[modConfigNTCGroupHiAmpExt],25.0f,0);
	modHiAmpPackStateHandle->temperatures[5] = driverSWADC128D818GetTemperature(modHiAmpGeneralConfigHandle->NTC25DegResistance[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCTopResistor[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCBetaFactor[modConfigNTCGroupHiAmpExt],25.0f,1);
	modHiAmpPackStateHandle->temperatures[6] = driverSWADC128D818GetTemperature(modHiAmpGeneralConfigHandle->NTC25DegResistance[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCTopResistor[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCBetaFactor[modConfigNTCGroupHiAmpExt],25.0f,2);
	modHiAmpPackStateHandle->temperatures[7] = driverSWADC128D818GetTemperature(modHiAmpGeneralConfigHandle->NTC25DegResistance[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCTopResistor[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCBetaFactor[modConfigNTCGroupHiAmpExt],25.0f,3);
	modHiAmpPackStateHandle->temperatures[8] = driverSWADC128D818GetTemperature(modHiAmpGeneralConfigHandle->NTC25DegResistance[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCTopResistor[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCBetaFactor[modConfigNTCGroupHiAmpExt],25.0f,4);
	modHiAmpPackStateHandle->temperatures[9] = driverSWADC128D818GetTemperature(modHiAmpGeneralConfigHandle->NTC25DegResistance[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCTopResistor[modConfigNTCGroupHiAmpExt],modHiAmpGeneralConfigHandle->NTCBetaFactor[modConfigNTCGroupHiAmpExt],25.0f,5);

	// Measure HiAmpshield NTC's (on the PCB)
	modHiAmpPackStateHandle->temperatures[10] = driverSWADC128D818GetTemperature(modHiAmpGeneralConfigHandle->NTC25DegResistance[modConfigNTCGroupHiAmpPCB],modHiAmpGeneralConfigHandle->NTCTopResistor[modConfigNTCGroupHiAmpPCB],modHiAmpGeneralConfigHandle->NTCBetaFactor[modConfigNTCGroupHiAmpPCB],25.0f,6);
	modHiAmpPackStateHandle->temperatures[11] = driverSWADC128D818GetTemperature(modHiAmpGeneralConfigHandle->NTC25DegResistance[modConfigNTCGroupHiAmpPCB],modHiAmpGeneralConfigHandle->NTCTopResistor[modConfigNTCGroupHiAmpPCB],modHiAmpGeneralConfigHandle->NTCBetaFactor[modConfigNTCGroupHiAmpPCB],25.0f,7);
		
	// Measure Aux NTC, this one is on the CAN connector
	if(modHiAmpPackStateHandle->slaveShieldPresenceAuxADC){
	  modHiAmpPackStateHandle->temperatures[13] = driverSWMCP3221GetTemperature(modHiAmpGeneralConfigHandle->NTC25DegResistance[modConfigNTCGroupHiAmpAUX],modHiAmpGeneralConfigHandle->NTCTopResistor[modConfigNTCGroupHiAmpAUX],modHiAmpGeneralConfigHandle->NTCBetaFactor[modConfigNTCGroupHiAmpAUX],25.0f);
	}
		
	// Read Temp and Humidity from SHT21 when ready
	if(driverSWSHT21PollMeasureReady()){
		modHiAmpPackStateHandle->temperatures[12] = driverSWSHT21GetTemperature();
		modHiAmpPackStateHandle->humidity         = driverSWSHT21GetHumidity();
	}
	
	if(modDelayTick1ms(&measureSHTStartLastTick,500)){
		driverSWSHT21StartMeasurement(lastMeasuredType);
		
		if(lastMeasuredType == TEMP)																																							// Toggle between SHT21 sensor modes
			lastMeasuredType = HUMIDITY;
		else
			lastMeasuredType = TEMP;
	}
}

void  modHiAmpShieldResetSensors(void) {		
	modHiAmpPackStateHandle->hiCurrentLoadVoltage         = 0.0f;
	modHiAmpPackStateHandle->hiCurrentLoadCurrent         = 0.0f;
	modHiAmpPackStateHandle->hiCurrentLoadPower           = 0.0f;
	modHiAmpPackStateHandle->auxVoltage                   = 0.0f;
	modHiAmpPackStateHandle->auxCurrent                   = 0.0f;
	modHiAmpPackStateHandle->auxPower                     = 0.0f;
	modHiAmpPackStateHandle->aux0LoadIncorrect            = false;
	modHiAmpPackStateHandle->aux1LoadIncorrect            = false;
	modHiAmpPackStateHandle->auxDCDCOutputOK              = false;
	modHiAmpPackStateHandle->temperatures[4]              = -50.0f;
	modHiAmpPackStateHandle->temperatures[5]              = -50.0f;
	modHiAmpPackStateHandle->temperatures[6]              = -50.0f;
	modHiAmpPackStateHandle->temperatures[7]              = -50.0f;
	modHiAmpPackStateHandle->temperatures[8]              = -50.0f;
	modHiAmpPackStateHandle->temperatures[9]              = -50.0f;
	modHiAmpPackStateHandle->temperatures[10]             = -50.0f;
	modHiAmpPackStateHandle->temperatures[11]             = -50.0f;
	modHiAmpPackStateHandle->temperatures[12]             = -50.0f;
	modHiAmpPackStateHandle->temperatures[13]             = -50.0f;
	modHiAmpPackStateHandle->humidity                     = 0.0f;
	modHiAmpPackStateHandle->hiLoadEnabled                = false;
	modHiAmpPackStateHandle->hiLoadPreChargeEnabled       = false;
	modHiAmpPackStateHandle->hiLoadPreChargeError         = false;
	modHiAmpPackStateHandle->IOIN1                        = false;
	modHiAmpPackStateHandle->IOOUT0                       = false;
	modHiAmpPackStateHandle->FANStatus.FANError           = false;
	modHiAmpPackStateHandle->FANStatus.FANOK              = false;
	modHiAmpPackStateHandle->FANStatus.FANSpeedRPM[0]     = 0;
	modHiAmpPackStateHandle->FANStatus.FANSpeedRPM[1]     = 0;
	modHiAmpPackStateHandle->FANStatus.FANSpeedRPM[2]     = 0;
	modHiAmpPackStateHandle->FANStatus.FANSpeedRPM[3]     = 0;
}

void modHiAmpShieldHVSSRTask(void) {
  driverSWPCAL6416SetOutput(0,0,HVEnableDischarge,false);
  driverSWPCAL6416SetOutput(0,1,HVEnablePreCharge,false);
  driverSWPCAL6416SetOutput(0,2,HVEnableLowSide,false);
  driverSWPCAL6416SetOutput(1,5,HVEnableCharge,true);
}

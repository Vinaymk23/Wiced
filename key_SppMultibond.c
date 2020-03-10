
#include "wiced.h"
#include "wiced_bt_dev.h"
#include "wiced_bt_ble.h"
#include "wiced_bt_uuid.h"
#include "wiced_bt_gatt.h"
#include "wiced_hal_nvram.h"
#include "wiced_hal_gpio.h"
#include "wiced_bt_app_hal_common.h"
#include "wiced_hal_platform.h"
#include "wiced_bt_trace.h"
#include "sparcommon.h"
#include "hci_control_api.h"
#include "wiced_transport.h"
#include "wiced_hal_pspi.h"
#include "key_SppMultibond_sdp_db.h"
#include "wiced_bt_cfg.h"
#include "wiced_bt_stack.h"
#include "wiced_bt_sdp.h"
#include "wiced_hal_wdog.h"
#include "wiced_bt_app_common.h"
#include "spp.h"
#include "wiced_hal_puart.h"
#include "wiced_rtos.h"


/*******************************************************************
 * Constant Definitions
 ******************************************************************/
#define TRANS_UART_BUFFER_SIZE  1024
#define TRANS_UART_BUFFER_COUNT 2

/* MultiBond */
// BOND_VSID holds the number of bonded devices
// Each VSID past there up to BOND_VSID+1+BOND_MAX holds one set of keys
#define BOND_VSID   WICED_NVRAM_VSID_START
#define BOND_MAX    8

/* SPI Pin Definition */
#define CS WICED_P07        //d10
#define DATAOUT WICED_P28   //d11 ->mosi
#define DATAIN WICED_P01    //d12 ->miso
#define SPICLOCK WICED_P38  //d13

/*Decoder and ADC Pin definitions*/
const uint8_t decoder_channel[3]={WICED_P02,WICED_P04,WICED_P06}; //d6,d7,d8


/*******************************************************************
 * Variable Definitions
 ******************************************************************/
extern const wiced_bt_cfg_settings_t wiced_bt_cfg_settings;
extern const wiced_bt_cfg_buf_pool_t wiced_bt_cfg_buf_pools[WICED_BT_CFG_NUM_BUF_POOLS];
// Transport pool for sending RFCOMM data to host
static wiced_transport_buffer_pool_t* transport_pool = NULL;

wiced_bt_device_address_t confirmAddr;

wiced_bool_t doCompare;

uint8_t numBonded=0;

/*Variables used in data transmission*/
uint8_t check_for_transmission=0;
uint8_t boolean=0;
wiced_thread_t *transmit_data_thread;

static uint16_t z=350;
uint16_t buffer=0;
uint8_t data_start=0;
uint8_t n_ack=0;
/*******************************************************************
 * Function Prototypes
 ******************************************************************/
static void                  key_classicspp_app_init            ( void );
static wiced_bt_dev_status_t key_classicspp_management_callback ( wiced_bt_management_evt_t event, wiced_bt_management_evt_data_t *p_event_data );
static void                  key_classicspp_reset_device        ( void );
static uint32_t              hci_control_process_rx_cmd         ( uint8_t* p_data, uint32_t len );
#ifdef HCI_TRACE_OVER_TRANSPORT
static void                  key_classicspp_trace_callback      ( wiced_bt_hci_trace_type_t type, uint16_t length, uint8_t* p_data );
#endif
int key_classicspp_read_link_keys( wiced_bt_device_link_keys_t *keys );
int key_classicspp_write_link_keys( wiced_bt_device_link_keys_t *keys );
void rx_cback( void *data );
/* MultiBond */
uint8_t readNumBonded(void);
void saveNumBonded(uint8_t bondedDevices);
void dumpLinkKeys(uint8_t bondedDevices);

/* SPI ReadData and SendData*/
uint8_t read_adc(uint8_t channel);
void button_cback( void *data, uint8_t port_pin );
void transmit_data(uint32_t arg);
void initialize_thread(void);

/*Selecting Pin Outputs for ADC and Decoder*/
void initialize_output_pins(void);
void decoder_select(uint8_t pin);
void send_data(uint16_t readvalue );

/*******************************************************************
 * Macro Definitions
 ******************************************************************/
// Macro to extract uint16_t from little-endian byte array
#define LITTLE_ENDIAN_BYTE_ARRAY_TO_UINT16(byte_array) \
        (uint16_t)( ((byte_array)[0] | ((byte_array)[1] << 8)) )

/*******************************************************************
 * Transport Configuration
 ******************************************************************/
wiced_transport_cfg_t transport_cfg =
{
    WICED_TRANSPORT_UART,              /**< Wiced transport type. */
    {
        WICED_TRANSPORT_UART_HCI_MODE, /**<  UART mode, HCI or Raw */
        HCI_UART_DEFAULT_BAUD          /**<  UART baud rate */
    },
    {
        TRANS_UART_BUFFER_SIZE,        /**<  Rx Buffer Size */
        TRANS_UART_BUFFER_COUNT        /**<  Rx Buffer Count */
    },
    NULL,                              /**< Wiced transport status handler.*/
    hci_control_process_rx_cmd,        /**< Wiced transport receive data handler. */
    NULL                               /**< Wiced transport tx complete callback. */
};

/*******************************************************************
 * Function Definitions
 ******************************************************************/

/*
 * Entry point to the application. Set device configuration and start BT
 * stack initialization.  The actual application initialization will happen
 * when stack reports that BT device is ready
 */

void initialize_output_pins(void)
{
    wiced_hal_gpio_configure_pin(WICED_P02,GPIO_OUTPUT_ENABLE,GPIO_PIN_OUTPUT_LOW);
    wiced_hal_gpio_configure_pin(WICED_P04,GPIO_OUTPUT_ENABLE,GPIO_PIN_OUTPUT_LOW);
    wiced_hal_gpio_configure_pin(WICED_P06,GPIO_OUTPUT_ENABLE,GPIO_PIN_OUTPUT_LOW);
}

void initialize_thread(void)
{
    transmit_data_thread=wiced_rtos_create_thread();
    wiced_rtos_init_thread(transmit_data_thread,3,"transmit_data",transmit_data,1000,NULL);
}

void application_start(void)
{
    /* Initialize the transport configuration */
    wiced_transport_init( &transport_cfg );

    /* Initialize Transport Buffer Pool */
    transport_pool = wiced_transport_create_buffer_pool ( TRANS_UART_BUFFER_SIZE, TRANS_UART_BUFFER_COUNT );

#if ((defined WICED_BT_TRACE_ENABLE) || (defined HCI_TRACE_OVER_TRANSPORT))
    /* Set the Debug UART as WICED_ROUTE_DEBUG_NONE to get rid of prints */
    //  wiced_set_debug_uart( WICED_ROUTE_DEBUG_NONE );

    /* Set Debug UART as WICED_ROUTE_DEBUG_TO_PUART to see debug traces on Peripheral UART (PUART) */
    wiced_set_debug_uart( WICED_ROUTE_DEBUG_TO_PUART );

    /* Set the Debug UART as WICED_ROUTE_DEBUG_TO_WICED_UART to send debug strings over the WICED debug interface */
    //wiced_set_debug_uart( WICED_ROUTE_DEBUG_TO_WICED_UART );
#endif

    /* Initialize Bluetooth Controller and Host Stack */
    wiced_bt_stack_init(key_classicspp_management_callback, &wiced_bt_cfg_settings, wiced_bt_cfg_buf_pools);
}

/*
 * This function is executed in the BTM_ENABLED_EVT management callback.
 */
void key_classicspp_app_init(void)
{
    /* Initialize Application */
    wiced_bt_app_init();

    /* MultiBond: Load number of bonded devices from NVRAM */
    numBonded = readNumBonded();

    /* Allow peer to pair */
    wiced_bt_set_pairable_mode(WICED_TRUE, 0);

    /* Initialize SDP Database */
    wiced_bt_sdp_db_init( (uint8_t*)sdp_database, sdp_database_len );

    /* Make device connectable (enables page scan) using default connectability window/interval.
     * The corresponding parameters are contained in 'wiced_bt_cfg.c' */
    /* TODO: Make sure that this is the desired behavior. */
    wiced_bt_dev_set_connectability(BTM_CONNECTABLE, BTM_DEFAULT_CONN_WINDOW, BTM_DEFAULT_CONN_INTERVAL);

    /* Make device discoverable (enables inquiry scan) over BR/EDR using default discoverability window/interval.
     * The corresponding parameters are contained in 'wiced_bt_cfg.c' */
    /* TODO: Make sure that this is the desired behavior. */
    wiced_bt_dev_set_discoverability(BTM_GENERAL_DISCOVERABLE, BTM_DEFAULT_DISC_WINDOW, BTM_DEFAULT_DISC_INTERVAL);

    /* Start the SPP server */
    spp_start();

    /*Configuring Push Button as an interrupt to send the data*/
    wiced_hal_gpio_register_pin_for_interrupt( WICED_GPIO_PIN_BUTTON_1, button_cback, NULL );
    wiced_hal_gpio_configure_pin( WICED_GPIO_PIN_BUTTON_1, ( GPIO_INPUT_ENABLE | GPIO_PULL_UP | GPIO_EN_INT_FALLING_EDGE ), GPIO_PIN_OUTPUT_HIGH );

    /*Initialize the Thread and output pins to read data handle data transmission and disable the watchdog timer*/
    initialize_thread();
    initialize_output_pins();
    wiced_hal_wdog_disable();


    /* Setup UART to receive characters */
    wiced_hal_puart_init( );
    wiced_hal_puart_flow_off( );
    wiced_hal_puart_set_baudrate( 115200 );
    wiced_hal_puart_enable_tx( );

    /* Enable receive and the interrupt */
    wiced_hal_puart_register_interrupt( rx_cback );

    /* Set watermark level to 1 to receive interrupt up on receiving each byte */
    wiced_hal_puart_set_watermark_level( 1 );
    wiced_hal_puart_enable_rx();
}

/* TODO: This function should be called when the device needs to be reset */
void key_classicspp_reset_device( void )
{
    /* TODO: Clear any additional persistent values used by the application from NVRAM */

    // Reset the device
    wiced_hal_wdog_reset_system( );


}

/* Bluetooth Management Event Handler */
wiced_bt_dev_status_t key_classicspp_management_callback( wiced_bt_management_evt_t event, wiced_bt_management_evt_data_t *p_event_data )
{
    wiced_bt_dev_status_t status = WICED_BT_SUCCESS;
    wiced_bt_device_address_t bda = { 0 };
    wiced_bt_dev_br_edr_pairing_info_t *p_br_edr_info = NULL;
    wiced_bt_ble_advert_mode_t *p_adv_mode = NULL;

    switch (event)
    {
    case BTM_ENABLED_EVT:
        /* Bluetooth Controller and Host Stack Enabled */

#ifdef HCI_TRACE_OVER_TRANSPORT
        // There is a virtual HCI interface between upper layers of the stack and
        // the controller portion of the chip with lower layers of the BT stack.
        // Register with the stack to receive all HCI commands, events and data.
        wiced_bt_dev_register_hci_trace(key_classicspp_trace_callback);
#endif

        WICED_BT_TRACE("Bluetooth Enabled (%s)\n",
                ((WICED_BT_SUCCESS == p_event_data->enabled.status) ? "success" : "failure"));

        if (WICED_BT_SUCCESS == p_event_data->enabled.status)
        {
            /* Bluetooth is enabled */
            wiced_bt_dev_read_local_addr(bda);
            WICED_BT_TRACE("Local Bluetooth Address: [%B]\n", bda);

            /* Perform application-specific initialization */
            key_classicspp_app_init();
        }
        break;
    case BTM_DISABLED_EVT:
        /* Bluetooth Controller and Host Stack Disabled */
        WICED_BT_TRACE("Bluetooth Disabled\n");
        break;
    case BTM_PASSKEY_NOTIFICATION_EVT: /* Print passkey to the screen so that the user can enter it. */
        WICED_BT_TRACE( "Passkey Notification\n\r");
        WICED_BT_TRACE(">>>>>>>>>>>>>>>>>>>>>>>> PassKey Required for BDA %B, Enter Key: %06d \n\r", p_event_data->user_passkey_notification.bd_addr, p_event_data->user_passkey_notification.passkey );
        break;
    case BTM_SECURITY_REQUEST_EVT:
        /* Security Request */
        WICED_BT_TRACE("Security Request\n");
        wiced_bt_ble_security_grant(p_event_data->security_request.bd_addr, WICED_BT_SUCCESS);
        break;
    case BTM_PAIRING_IO_CAPABILITIES_BR_EDR_RESPONSE_EVT:
         WICED_BT_TRACE(
                         "IO_CAPABILITIES_BR_EDR_RESPONSE peer_bd_addr: %B, peer_io_cap: %d, peer_oob_data: %d, peer_auth_req: %d\n",
                         p_event_data->pairing_io_capabilities_br_edr_response.bd_addr,
                         p_event_data->pairing_io_capabilities_br_edr_response.io_cap,
                         p_event_data->pairing_io_capabilities_br_edr_response.oob_data,
                         p_event_data->pairing_io_capabilities_br_edr_response.auth_req);
         break;
    case BTM_PAIRING_IO_CAPABILITIES_BR_EDR_REQUEST_EVT:
        /* Request for Pairing IO Capabilities (BR/EDR) */
        WICED_BT_TRACE("BR/EDR Pairing IO Capabilities Request\n");
        p_event_data->pairing_io_capabilities_br_edr_request.oob_data = BTM_OOB_NONE;
        p_event_data->pairing_io_capabilities_br_edr_request.auth_req = BTM_AUTH_SINGLE_PROFILE_GENERAL_BONDING_YES;
        p_event_data->pairing_io_capabilities_br_edr_request.is_orig = WICED_FALSE;
        p_event_data->pairing_io_capabilities_br_edr_request.local_io_cap = BTM_IO_CAPABILITIES_DISPLAY_AND_YES_NO_INPUT;
        break;
    case BTM_PAIRING_COMPLETE_EVT:
        /* Pairing is Complete */
        p_br_edr_info = &p_event_data->pairing_complete.pairing_complete_info.br_edr;
        WICED_BT_TRACE("Pairing Complete %d.\n", p_br_edr_info->status);
        break;
    case BTM_ENCRYPTION_STATUS_EVT:
        /* Encryption Status Change */
        WICED_BT_TRACE("Encryption Status event: bd ( %B ) res %d\n", p_event_data->encryption_status.bd_addr, p_event_data->encryption_status.result);
        break;
    case BTM_PAIRED_DEVICE_LINK_KEYS_REQUEST_EVT:
        /* Paired Device Link Keys Request */
        WICED_BT_TRACE("Paired Device Link Request Keys Event\n");
        /* Device/app-specific TODO: HANDLE PAIRED DEVICE LINK REQUEST KEY - retrieve from NVRAM, etc */
#if 1
        if (key_classicspp_read_link_keys( &p_event_data->paired_device_link_keys_request ))
        {
            WICED_BT_TRACE("Key Retrieval Success\n");
        }
        else
#endif
        /* Until key retrieval implemented above, just fail the request - will cause re-pairing */
        {
            WICED_BT_TRACE("Key Retrieval Failure\n");
            status = WICED_BT_ERROR;
        }
        break;
    case BTM_PAIRED_DEVICE_LINK_KEYS_UPDATE_EVT:
        WICED_BT_TRACE("BTM_PAIRED_DEVICE_LINK_KEYS_UPDATE_EVT\n");
        key_classicspp_write_link_keys( &p_event_data->paired_device_link_keys_update);
        break;
    case BTM_USER_CONFIRMATION_REQUEST_EVT:
        /* Pairing request, TODO: handle confirmation of numeric compare here if desired */
        WICED_BT_TRACE("numeric_value: %d\n", p_event_data->user_confirmation_request.numeric_value);
        WICED_BT_TRACE("Press 'y' to accept the connection or 'n' to reject\n");

        /* Save the BDADDR */
        memcpy(confirmAddr,p_event_data->user_confirmation_request.bd_addr,sizeof(confirmAddr));

        doCompare = WICED_TRUE;

        /* This is done in the  callback based on the key press */
        //wiced_bt_dev_confirm_req_reply( WICED_BT_SUCCESS , p_event_data->user_confirmation_request.bd_addr);
        break;
    default:
        WICED_BT_TRACE("Unhandled Bluetooth Management Event: 0x%x (%d)\n", event, event);
//        transmit_data();
        break;
    }

    return status;
}


/* Handle Command Received over Transport */
uint32_t hci_control_process_rx_cmd( uint8_t* p_data, uint32_t len )
{
    uint8_t status = 0;
    uint8_t cmd_status = HCI_CONTROL_STATUS_SUCCESS;
    uint8_t opcode = 0;
    uint8_t* p_payload_data = NULL;
    uint8_t payload_length = 0;

    WICED_BT_TRACE("hci_control_process_rx_cmd : Data Length '%d'\n", len);

    // At least 4 bytes are expected in WICED Header
    if ((NULL == p_data) || (len < 4))
    {
        WICED_BT_TRACE("Invalid Parameters\n");
        status = HCI_CONTROL_STATUS_INVALID_ARGS;
    }
    else
    {
        // Extract OpCode and Payload Length from little-endian byte array
        opcode = LITTLE_ENDIAN_BYTE_ARRAY_TO_UINT16(p_data);
        payload_length = LITTLE_ENDIAN_BYTE_ARRAY_TO_UINT16(&p_data[sizeof(uint16_t)]);
        p_payload_data = &p_data[sizeof(uint16_t)*2];

        // TODO : Process received HCI Command based on its Control Group
        // (see 'hci_control_api.h' for additional details)
        switch ( HCI_CONTROL_GROUP(opcode) )
        {
        default:
            // HCI Control Group was not handled
            cmd_status = HCI_CONTROL_STATUS_UNKNOWN_GROUP;
            wiced_transport_send_data(HCI_CONTROL_EVENT_COMMAND_STATUS, &cmd_status, sizeof(cmd_status));
            break;
        }
    }

    // When operating in WICED_TRANSPORT_UART_HCI_MODE or WICED_TRANSPORT_SPI,
    // application has to free buffer in which data was received
    wiced_transport_free_buffer( p_data );
    p_data = NULL;

    return status;
}

#ifdef HCI_TRACE_OVER_TRANSPORT

/* Handle Sending of Trace over the Transport */
void key_classicspp_trace_callback( wiced_bt_hci_trace_type_t type, uint16_t length, uint8_t* p_data )
{

    wiced_transport_send_hci_trace( transport_pool, type, length, p_data );

}
#endif


// This function reads the first VSID row into the link keys
int key_classicspp_read_link_keys( wiced_bt_device_link_keys_t *keys )
{
    wiced_result_t result;
    int bytes_read;
    wiced_bt_device_link_keys_t tempKeys;

    WICED_BT_TRACE("Searching for Link Keys %B\n",keys->bd_addr);

    for(uint32_t i=0;i<numBonded;i++)
    {
      bytes_read = wiced_hal_read_nvram(BOND_VSID+1+i,sizeof(wiced_bt_device_link_keys_t),
            (uint8_t *)&tempKeys,&result);

      WICED_BT_TRACE("Read keys for %B\n",tempKeys.bd_addr);

      if(memcmp(keys->bd_addr,tempKeys.bd_addr,sizeof(wiced_bt_device_address_t)) == 0)
      {
          memcpy(keys,&tempKeys,sizeof(wiced_bt_device_link_keys_t));
          WICED_BT_TRACE("Found Keys Match VSID=%d\n", BOND_VSID+1+i);
          return BOND_VSID+1+i;
      }
    }
    WICED_BT_TRACE("Failed to find Link Keys\n");
    return WICED_NVRAM_VSID_END+1;
}

// This function write the link keys into the first VSID row
int key_classicspp_write_link_keys( wiced_bt_device_link_keys_t *keys )
{
    wiced_result_t  result;
    wiced_bt_device_link_keys_t tempKeys;

    memcpy(&tempKeys.bd_addr,keys->bd_addr,sizeof(wiced_bt_device_address_t));

    uint32_t vsid = key_classicspp_read_link_keys(&tempKeys);
    if(vsid>WICED_NVRAM_VSID_END)
    {
        vsid = numBonded + BOND_VSID+1;
        numBonded = numBonded + 1;
        if(numBonded > BOND_MAX)
        {
            WICED_BT_TRACE("Bonding Table Full\n");
            return WICED_ERROR;
        }
        saveNumBonded(numBonded);
    }

    WICED_BT_TRACE("Writing Link Keys for %B Current Bonded =%d VSID=%d\n",keys->bd_addr,numBonded,vsid);

    int   bytes_written = wiced_hal_write_nvram(vsid, sizeof(wiced_bt_device_link_keys_t),
          (uint8_t*)keys, &result);

    dumpLinkKeys(numBonded);
    return result;
}


/* Interrupt callback function for UART */
void rx_cback( void *data )
{

    uint8_t  readbyte;
    /* Read one byte from the buffer and (unlike GPIO) reset the interrupt */
    wiced_hal_puart_read( &readbyte );
    wiced_hal_puart_reset_puart_interrupt();

    if(doCompare == WICED_TRUE)
    {
        /* Send appropriate response */
        if(readbyte == 'y') /* Yes */
        {
            wiced_bt_dev_confirm_req_reply( WICED_BT_SUCCESS , confirmAddr);
            WICED_BT_TRACE("Confirm Accepted\n");
        }
        if(readbyte == 'n') /* No */
        {
            wiced_bt_dev_confirm_req_reply( WICED_BT_ERROR , confirmAddr);
            WICED_BT_TRACE("Confirm Failed\n");
        }
        /* Disable compare input checking */
        doCompare = WICED_FALSE;
    }
    else
    {

    }
}


/* MultiBond */
// This function returns the number of devices bonded.  The number is a uint8_t that is stored in the first VSID
// If there is nothing stored in the first VSID then it initializes that number to 0
uint8_t readNumBonded(void)
{
    int bytes_read;
    wiced_result_t result;
    uint8_t count;
    uint8_t bondedDevices;

    bytes_read = wiced_hal_read_nvram(BOND_VSID,sizeof(uint8_t),&bondedDevices,&result);

    if(result != WICED_SUCCESS)
    {
        WICED_BT_TRACE("Initializing bonding table\n");
        bondedDevices = 0;
        wiced_hal_write_nvram(BOND_VSID, sizeof(uint8_t),&bondedDevices, &result);
    }

    WICED_BT_TRACE("Read Number bonded = %d\n",bondedDevices);

    return bondedDevices;
}

/* MultiBond */
// This function saves the argument bondedDevices to BOND_VSID
void saveNumBonded(uint8_t bondedDevices)
{
    WICED_BT_TRACE("Updating Bonding Count = %d\n",bondedDevices);
    wiced_result_t result;
    wiced_hal_write_nvram(BOND_VSID, sizeof(uint8_t), (uint8_t*)&bondedDevices, &result);
}

/* MultiBond */
// This function dumps a print of all of the keys to the BT Trace UART
void dumpLinkKeys(uint8_t bondedDevices)
{
        wiced_result_t result;
        int bytes_read;
        wiced_bt_device_link_keys_t tempKeys;

        WICED_BT_TRACE("-----------------\nDumping Link Keys Table\n");
        for(uint32_t i=0;i<bondedDevices;i++)
        {
          bytes_read = wiced_hal_read_nvram(BOND_VSID+1+i,sizeof(wiced_bt_device_link_keys_t),
                (uint8_t *)&tempKeys,&result);

          WICED_BT_TRACE("Read keys for %B in VSID=%d Result=%d ByteRead=%d\n",tempKeys.bd_addr,BOND_VSID+1+i,result,bytes_read);
        }
        WICED_BT_TRACE("----------------\n");
}


void button_cback( void *data, uint8_t port_pin )
{
    wiced_hal_gpio_clear_pin_interrupt_status( WICED_GPIO_PIN_BUTTON_1 );
    check_for_transmission = !check_for_transmission;
//        wiced_rtos_delay_milliseconds(500,1);
    if(check_for_transmission)
    {
        WICED_BT_TRACE("=========Data Transmission Possible=======\n\r");
        boolean=255;
    }
    else
        WICED_BT_TRACE("==========Data Transmission Not Possible=======\n\r");

}

void transmit_data(uint32_t arg)
{
    WICED_BT_TRACE("------inside thread function-------\n\r");
    uint8_t newline=10;
    char ready='y';
    while(1)
    {
    if(!check_for_transmission)
    {
//        WICED_BT_TRACE("Data Transmission Not Possible\n\r");
        wiced_hal_gpio_set_pin_output(WICED_GPIO_PIN_LED_2,0);
        wiced_rtos_delay_milliseconds(250,ALLOW_THREAD_TO_SLEEP);

    }
    else if(check_for_transmission)
    {
//        WICED_BT_TRACE("Data Transmission Possible,Waiting for clearance\n\r");
        wiced_hal_gpio_set_pin_output(WICED_GPIO_PIN_LED_2,1);
        if(data_start)
        {
            for(uint8_t i=0;i<8;i++) // decoder outputs 0 to 7
            {
                WICED_BT_TRACE("inside decoder select %d\n\r",i);
                decoder_select(i);
            }
        data_start=0;
        }
    }
    }
}

void decoder_select(uint8_t pin)
{
    for(uint8_t i=0;i<3;i++)
    {
        if(pin&(1<<i))
        {
            wiced_hal_gpio_set_pin_output(decoder_channel[i],GPIO_PIN_OUTPUT_HIGH);
            WICED_BT_TRACE("if pin =%d  data=%d\n\r",(pin&(1<<i)),wiced_hal_gpio_get_pin_output(decoder_channel[i]));
        }
        else
        {
            wiced_hal_gpio_set_pin_output(decoder_channel[i],GPIO_PIN_OUTPUT_LOW);
            WICED_BT_TRACE("else pin =%d  data=%d\n\r",(pin&(1<<i)),wiced_hal_gpio_get_pin_output(decoder_channel[i]));
        }
    }
    uint8_t temp=0;
    for(uint8_t i=1;i<9;i++) // adc channels 0 to 7
    {
        WICED_BT_TRACE("inside ADC select %d\n\r",i);
        temp=read_adc(i);
        WICED_BT_TRACE("adc return value=%d-----------------\n\r",temp);
            if(temp)
            {
                WICED_BT_TRACE("inside read_Adc if\n\r");
                i--;
            }
    }
}

uint8_t read_adc(uint8_t channel)
{
    WICED_BT_TRACE("inside read_Adc function\n\r");
    uint8_t resend=0;
    uint16_t readvalue=0;
    uint8_t commandbits=0b11000000; //command bits - start(1), mode(1), channels (3), dont care (3)
    commandbits|=((channel-1)<<3);
    wiced_hal_gpio_set_pin_output(CS, GPIO_PIN_OUTPUT_LOW);
    uint8_t set_DATAOUT=0;
    for(uint8_t i=7;i>=3;i--)
    {
        set_DATAOUT=commandbits&(1<<i);
        if(set_DATAOUT)
           set_DATAOUT=1;
        else
           set_DATAOUT=0;
        wiced_hal_gpio_set_pin_output(DATAOUT,set_DATAOUT);
          //cycle clock
        wiced_hal_gpio_set_pin_output(SPICLOCK,GPIO_PIN_OUTPUT_HIGH);
        wiced_hal_gpio_set_pin_output(SPICLOCK,GPIO_PIN_OUTPUT_LOW);
     }

//    ignores 2 null bits
     wiced_hal_gpio_set_pin_output(SPICLOCK,GPIO_PIN_OUTPUT_HIGH);
     wiced_hal_gpio_set_pin_output(SPICLOCK,GPIO_PIN_OUTPUT_LOW);
     wiced_hal_gpio_set_pin_output(SPICLOCK,GPIO_PIN_OUTPUT_HIGH);
     wiced_hal_gpio_set_pin_output(SPICLOCK,GPIO_PIN_OUTPUT_LOW);

     for(int i=11;i>=0;i--)
     {
       readvalue+=wiced_hal_gpio_get_pin_input_status(DATAIN)<<i;
//     cycle clock
       wiced_hal_gpio_set_pin_output(SPICLOCK,GPIO_PIN_OUTPUT_HIGH);
       wiced_hal_gpio_set_pin_output(SPICLOCK,GPIO_PIN_OUTPUT_LOW);
      }

     wiced_hal_gpio_set_pin_output(CS, GPIO_PIN_OUTPUT_HIGH);
     WICED_BT_TRACE("readvalue=%d--------n_ack:%d\n\r",readvalue,n_ack);
     if(!n_ack)
     {
//         z=z+10;
         buffer=readvalue;
         send_data(readvalue);
         WICED_BT_TRACE("-----inside p_ack------\n\r");
         resend=0;
     }
     else
     {
         send_data(buffer);
         WICED_BT_TRACE("-----inside n_ack------\n\r");
         n_ack=0;
         resend=255;
     }
     return resend;
}

void send_data(uint16_t readvalue )
{
    uint16_t transmit_number=0;
    uint8_t remainder=0;
    uint8_t newline=10;
    /* Transmit the data using the Bluetooth SPP */

    /*To reverse digits to send one digit at a time*/
    for(uint8_t i=0;i<4;i++)
    {
        remainder=readvalue%10;
        transmit_number=transmit_number*10+remainder;
        readvalue/=10;
     }
     WICED_BT_TRACE("Reversed Number=%d\n\r",transmit_number);
     /*Sending one digit at a time*/
     for(uint8_t i=0;i<4;i++)
     {
        remainder=transmit_number%10;
        remainder+=48;
        WICED_BT_TRACE("transmitted digit=%d\t",remainder);
        spp_tx_data(&remainder, sizeof(remainder));
        transmit_number/=10;
      }
     spp_tx_data(&newline, sizeof(newline));
     wiced_rtos_delay_milliseconds(1,ALLOW_THREAD_TO_SLEEP);
     WICED_BT_TRACE("-----finished data transmission------\n\r");
}



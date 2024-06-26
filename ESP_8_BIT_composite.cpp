/* Copyright (c) 2020, Peter Barrett
**
** Permission to use, copy, modify, and/or distribute this software for
** any purpose with or without fee is hereby granted, provided that the
** above copyright notice and this permission notice appear in all copies.
**
** THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
** WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
** WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR
** BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES
** OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
** WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
** ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
** SOFTWARE.
*/

/*
** Extracted from Peter Barrett's ESP_8_BIT project and adapted to Arduino
** library by Roger Cheng
*/

#include "ESP_8_BIT_composite.h"

static const char *TAG = "ESP_8_BIT";

static ESP_8_BIT_composite* _instance_ = NULL;
static int _pal_ = 0;

//====================================================================================================
//====================================================================================================
//
// low level HW setup of DAC/DMA/APLL/PWM
//
#include "clk_ctrl_os.h"
lldesc_t _dma_desc[2] = {0};
intr_handle_t _isr_handle;

#ifdef CONFIG_IDF_TARGET_ESP32S2
// Naming convention: SOC_MOD_CLK_{[upstream]clock_name}_[attr]
// {[upstream]clock_name}: APB, APLL, (BB)PLL, etc.
// [attr] - optional: FAST, SLOW, D<divider>, F<freq>
/**
 * @brief Supported clock sources for modules (CPU, peripherals, RTC, etc.)
 *
 * @note enum starts from 1, to save 0 for special purpose
 */
typedef enum {
    // For CPU domain
    SOC_MOD_CLK_CPU = 1,                       /*!< CPU_CLK can be sourced from XTAL, PLL, RC_FAST, or APLL by configuring soc_cpu_clk_src_t */
    // For RTC domain
    SOC_MOD_CLK_RTC_FAST,                      /*!< RTC_FAST_CLK can be sourced from XTAL_D4 or RC_FAST by configuring soc_rtc_fast_clk_src_t */
    SOC_MOD_CLK_RTC_SLOW,                      /*!< RTC_SLOW_CLK can be sourced from RC_SLOW, XTAL32K, or RC_FAST_D256 by configuring soc_rtc_slow_clk_src_t */
    // For digital domain: peripherals, WIFI, BLE
    SOC_MOD_CLK_APB,                           /*!< APB_CLK is highly dependent on the CPU_CLK source */
    SOC_MOD_CLK_PLL_D2,                        /*!< PLL_D2_CLK is derived from PLL, it has a fixed divider of 2 */
    SOC_MOD_CLK_PLL_F160M,                     /*!< PLL_F160M_CLK is derived from PLL, and has a fixed frequency of 160MHz */
    SOC_MOD_CLK_XTAL32K,                       /*!< XTAL32K_CLK comes from the external 32kHz crystal, passing a clock gating to the peripherals */
    SOC_MOD_CLK_RC_FAST,                       /*!< RC_FAST_CLK comes from the internal 8MHz rc oscillator, passing a clock gating to the peripherals */
    SOC_MOD_CLK_RC_FAST_D256,                  /*!< RC_FAST_D256_CLK comes from the internal 8MHz rc oscillator, divided by 256, and passing a clock gating to the peripherals */
    SOC_MOD_CLK_XTAL,                          /*!< XTAL_CLK comes from the external crystal (2~40MHz) */
    SOC_MOD_CLK_REF_TICK,                      /*!< REF_TICK is derived from APB, it has a fixed frequency of 1MHz even when APB frequency changes */
    SOC_MOD_CLK_APLL,                          /*!< APLL is sourced from PLL, and its frequency is configurable through APLL configuration registers */
    SOC_MOD_CLK_INVALID,                       /*!< Indication of the end of the available module clock sources */
} soc_module_clk_t;

/**
 * @brief Integer division operation
 *
 */
typedef enum {
    HAL_DIV_ROUND_DOWN,     /*!< Round the division down to the floor integer */
    HAL_DIV_ROUND_UP,       /*!< Round the division up to the ceiling integer */
    HAL_DIV_ROUND,          /*!< Round the division to the nearest integer (round up if fraction >= 1/2, round down if fraction < 1/2) */
} hal_utils_div_round_opt_t;

/**
 * @brief Clock information
 *
 */
typedef struct {
    uint32_t src_freq_hz;   /*!< Source clock frequency, unit: Hz */
    uint32_t exp_freq_hz;   /*!< Expected output clock frequency, unit: Hz */
    uint32_t max_integ;     /*!< The max value of the integral part */
    uint32_t min_integ;     /*!< The min value of the integral part, integer range: [min_integ, max_integ) */
    union {
        uint32_t max_fract;     /*!< The max value of the denominator and numerator, numerator range: [0, max_fract), denominator range: [1, max_fract)
                                 *   Please make sure max_fract > 2 when calculate the division with fractal part */
        hal_utils_div_round_opt_t round_opt;     /*!< Integer division operation. For the case that doesn't have fractal part, set this field to the to specify the rounding method  */
    };
} hal_utils_clk_info_t;

/**
 * @brief Members of clock division
 *
 */
typedef struct {
    uint32_t integer;       /*!< Integer part of division */
    uint32_t denominator;   /*!< Denominator part of division */
    uint32_t numerator;     /*!< Numerator part of division */
} hal_utils_clk_div_t;

/**
 * @brief Calculate the clock division with fractal part accurately
 * @note  Accuracy first algorithm, Time complexity O(n).
 *        About 1~hundreds times more accurate than the fast algorithm
 *
 * @param[in]  clk_info     The clock information
 * @param[out] clk_div      The clock division with integral and fractal part
 * @return
 *      - 0: Failed to get the result because the division is out of range
 *      - others: The real output clock frequency
 */
// uint32_t hal_utils_calc_clk_div_frac_accurate(const hal_utils_clk_info_t *clk_info, hal_utils_clk_div_t *clk_div);
typedef enum {
    ADC_DIGI_CLK_SRC_PLL_F160M = SOC_MOD_CLK_PLL_F160M, /*!< Select F160M as the source clock */
    ADC_DIGI_CLK_SRC_APLL = SOC_MOD_CLK_APLL,           /*!< Select APLL as the source clock */
    ADC_DIGI_CLK_SRC_DEFAULT = SOC_MOD_CLK_PLL_F160M,   /*!< Select F160M as the default clock choice */
} soc_periph_adc_digi_clk_src_t;

__attribute__((always_inline))
static inline uint32_t _sub_abs(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

uint32_t hal_utils_calc_clk_div_frac_accurate(const hal_utils_clk_info_t *clk_info, hal_utils_clk_div_t *clk_div)
{
    HAL_ASSERT(clk_info->max_fract > 2);
    uint32_t div_denom = 2;
    uint32_t div_numer = 0;
    uint32_t div_integ = clk_info->src_freq_hz / clk_info->exp_freq_hz;
    uint32_t freq_error = clk_info->src_freq_hz % clk_info->exp_freq_hz;

    if (freq_error) {
        // Carry bit if the decimal is greater than 1.0 - 1.0 / ((max_fract - 1) * 2)
        if (freq_error < clk_info->exp_freq_hz - clk_info->exp_freq_hz / (clk_info->max_fract - 1) * 2) {
            // Search the closest fraction, time complexity O(n)
            for (uint32_t sub = 0, a = 2, b = 0, min = UINT32_MAX; min && a < clk_info->max_fract; a++) {
                b = (a * freq_error + clk_info->exp_freq_hz / 2) / clk_info->exp_freq_hz;
                sub = _sub_abs(clk_info->exp_freq_hz * b, freq_error * a);
                if (sub < min) {
                    div_denom = a;
                    div_numer = b;
                    min = sub;
                }
            }
        } else {
            div_integ++;
        }
    }

    // If the expect frequency is too high or too low to satisfy the integral division range, failed and return 0
    if (div_integ < clk_info->min_integ || div_integ >= clk_info->max_integ || div_integ == 0) {
        return 0;
    }

    // Assign result
    clk_div->integer     = div_integ;
    clk_div->denominator = div_denom;
    clk_div->numerator   = div_numer;

    // Return the actual frequency
    if (div_numer) {
        uint32_t temp = div_integ * div_denom + div_numer;
        return (uint32_t)(((uint64_t)clk_info->src_freq_hz * div_denom + temp / 2) / temp);
    }
    return clk_info->src_freq_hz / div_integ;
}

static portMUX_TYPE periph_spinlock = portMUX_INITIALIZER_UNLOCKED;
/* APLL output frequency range */
#define CLK_LL_APLL_MIN_HZ    (5303031)   // 5.303031 MHz, refer to 'periph_rtc_apll_freq_set' for the calculation
#define CLK_LL_APLL_MAX_HZ    (125000000) // 125MHz, refer to 'periph_rtc_apll_freq_set' for the calculation

#define  APB_CLK_FREQ                                ( 80*1000000 )       //unit: Hz
// static const char *TAG = "DAC_DMA";

/* APLL configuration parameters */
#define CLK_LL_APLL_SDM_STOP_VAL_1         0x09
#define CLK_LL_APLL_SDM_STOP_VAL_2_REV0    0x69
#define CLK_LL_APLL_SDM_STOP_VAL_2_REV1    0x49

/* APLL calibration parameters */
#define CLK_LL_APLL_CAL_DELAY_1            0x0f
#define CLK_LL_APLL_CAL_DELAY_2            0x3f
#define CLK_LL_APLL_CAL_DELAY_3            0x1f

/* APLL multiplier output frequency range */
// apll_multiplier_out = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536)
#define CLK_LL_APLL_MULTIPLIER_MIN_HZ (350000000) // 350 MHz
#define CLK_LL_APLL_MULTIPLIER_MAX_HZ (500000000) // 500 MHz

#define I2C_APLL            0X6D
#define I2C_APLL_HOSTID     1

#define I2C_APLL_IR_CAL_DELAY        0
#define I2C_APLL_IR_CAL_DELAY_MSB    3
#define I2C_APLL_IR_CAL_DELAY_LSB    0

#define I2C_APLL_SDM_STOP        5
#define I2C_APLL_SDM_STOP_MSB    5
#define I2C_APLL_SDM_STOP_LSB    5

#define I2C_APLL_DSDM2        7
#define I2C_APLL_DSDM2_MSB    5
#define I2C_APLL_DSDM2_LSB    0

#define I2C_APLL_DSDM1        8
#define I2C_APLL_DSDM1_MSB    7
#define I2C_APLL_DSDM1_LSB    0

#define I2C_APLL_DSDM0        9
#define I2C_APLL_DSDM0_MSB    7
#define I2C_APLL_DSDM0_LSB    0

#define I2C_APLL_OR_OUTPUT_DIV        4
#define I2C_APLL_OR_OUTPUT_DIV_MSB    4
#define I2C_APLL_OR_OUTPUT_DIV_LSB    0

#define I2C_APLL_OR_CAL_END        3
#define I2C_APLL_OR_CAL_END_MSB    7
#define I2C_APLL_OR_CAL_END_LSB    7

/**
 * @brief Check whether APLL calibration is done
 *
 * @return True if calibration is done; otherwise false
 */
static inline __attribute__((always_inline)) bool clk_ll_apll_calibration_is_done(void)
{
    return REGI2C_READ_MASK(I2C_APLL, I2C_APLL_OR_CAL_END);
}

/**
 * @brief Set APLL calibration parameters
 */
static inline __attribute__((always_inline)) void clk_ll_apll_set_calibration(void)
{
    REGI2C_WRITE(I2C_APLL, I2C_APLL_IR_CAL_DELAY, CLK_LL_APLL_CAL_DELAY_1);
    REGI2C_WRITE(I2C_APLL, I2C_APLL_IR_CAL_DELAY, CLK_LL_APLL_CAL_DELAY_2);
    REGI2C_WRITE(I2C_APLL, I2C_APLL_IR_CAL_DELAY, CLK_LL_APLL_CAL_DELAY_3);
}


/**
 * @brief Set APLL configuration
 *
 * @param o_div  Frequency divider, 0..31
 * @param sdm0  Frequency adjustment parameter, 0..255
 * @param sdm1  Frequency adjustment parameter, 0..255
 * @param sdm2  Frequency adjustment parameter, 0..63
 */
static inline __attribute__((always_inline)) void clk_ll_apll_set_config(uint32_t o_div, uint32_t sdm0, uint32_t sdm1, uint32_t sdm2)
{
    REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_DSDM2, sdm2);
    REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_DSDM0, sdm0);
    REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_DSDM1, sdm1);
    REGI2C_WRITE(I2C_APLL, I2C_APLL_SDM_STOP, CLK_LL_APLL_SDM_STOP_VAL_1);
    REGI2C_WRITE(I2C_APLL, I2C_APLL_SDM_STOP, CLK_LL_APLL_SDM_STOP_VAL_2_REV1);
    REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_OR_OUTPUT_DIV, o_div);
}

static uint32_t s_cur_apll_freq = 0;
static int s_apll_ref_cnt = 0;

uint32_t rtc_clk_apll_coeff_calc(uint32_t freq, uint32_t *_o_div, uint32_t *_sdm0, uint32_t *_sdm1, uint32_t *_sdm2)
{
    uint32_t rtc_xtal_freq = (uint32_t)rtc_clk_xtal_freq_get();
    if (rtc_xtal_freq == 0) {
        // xtal_freq has not set yet
        // ESP_HW_LOGE(TAG, "Get xtal clock frequency failed, it has not been set yet");
        abort();
    }
    /* Reference formula: apll_freq = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536) / ((o_div + 2) * 2)
     *                                ----------------------------------------------   -----------------
     *                                     350 MHz <= Numerator <= 500 MHz                Denominator
     */
    int o_div = 0; // range: 0~31
    int sdm0 = 0;  // range: 0~255
    int sdm1 = 0;  // range: 0~255
    int sdm2 = 0;  // range: 0~63
    /* Firstly try to satisfy the condition that the operation frequency of numerator should be greater than 350 MHz,
     * i.e. xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536) >= 350 MHz, '+1' in the following code is to get the ceil value.
     * With this condition, as we know the 'o_div' can't be greater than 31, then we can calculate the APLL minimum support frequency is
     * 350 MHz / ((31 + 2) * 2) = 5303031 Hz (for ceil) */
    o_div = (int)(CLK_LL_APLL_MULTIPLIER_MIN_HZ / (float)(freq * 2) + 1) - 2;
    if (o_div > 31) {
        // ESP_HW_LOGE(TAG, "Expected frequency is too small");
        return 0;
    }
    if (o_div < 0) {
        /* Try to satisfy the condition that the operation frequency of numerator should be smaller than 500 MHz,
         * i.e. xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536) <= 500 MHz, we need to get the floor value in the following code.
         * With this condition, as we know the 'o_div' can't be smaller than 0, then we can calculate the APLL maximum support frequency is
         * 500 MHz / ((0 + 2) * 2) = 125000000 Hz */
        o_div = (int)(CLK_LL_APLL_MULTIPLIER_MAX_HZ / (float)(freq * 2)) - 2;
        if (o_div < 0) {
            // ESP_HW_LOGE(TAG, "Expected frequency is too big");
            return 0;
        }
    }
    // sdm2 = (int)(((o_div + 2) * 2) * apll_freq / xtal_freq) - 4
    sdm2 = (int)(((o_div + 2) * 2 * freq) / (rtc_xtal_freq * MHZ)) - 4;
    // numrator = (((o_div + 2) * 2) * apll_freq / xtal_freq) - 4 - sdm2
    float numrator = (((o_div + 2) * 2 * freq) / ((float)rtc_xtal_freq * MHZ)) - 4 - sdm2;
    // If numrator is bigger than 255/256 + 255/65536 + (1/65536)/2 = 1 - (1 / 65536)/2, carry bit to sdm2
    if (numrator > 1.0 - (1.0 / 65536.0) / 2.0) {
        sdm2++;
    }
    // If numrator is smaller than (1/65536)/2, keep sdm0 = sdm1 = 0, otherwise calculate sdm0 and sdm1
    else if (numrator > (1.0 / 65536.0) / 2.0) {
        // Get the closest sdm1
        sdm1 = (int)(numrator * 65536.0 + 0.5) / 256;
        // Get the closest sdm0
        sdm0 = (int)(numrator * 65536.0 + 0.5) % 256;
    }
    uint32_t real_freq = (uint32_t)(rtc_xtal_freq * MHZ * (4 + sdm2 + (float)sdm1/256.0 + (float)sdm0/65536.0) / (((float)o_div + 2) * 2));
    *_o_div = o_div;
    *_sdm0 = sdm0;
    *_sdm1 = sdm1;
    *_sdm2 = sdm2;
    return real_freq;
}

void rtc_clk_apll_coeff_set(uint32_t o_div, uint32_t sdm0, uint32_t sdm1, uint32_t sdm2)
{
    clk_ll_apll_set_config(o_div, sdm0, sdm1, sdm2);

    /* calibration */
    clk_ll_apll_set_calibration();

    /* wait for calibration end */
    while (!clk_ll_apll_calibration_is_done()) {
        /* use esp_rom_delay_us so the RTC bus doesn't get flooded */
        esp_rom_delay_us(1);
    }
}

esp_err_t periph_rtc_apll_freq_set(uint32_t expt_freq, uint32_t *real_freq)
{
    uint32_t o_div = 0;
    uint32_t sdm0 = 0;
    uint32_t sdm1 = 0;
    uint32_t sdm2 = 0;
    // Guarantee 'periph_rtc_apll_acquire' has been called before set apll freq
    assert(s_apll_ref_cnt > 0);
    uint32_t apll_freq = rtc_clk_apll_coeff_calc(expt_freq, &o_div, &sdm0, &sdm1, &sdm2);

    // ESP_RETURN_ON_FALSE(apll_freq, ESP_ERR_INVALID_ARG, TAG, "APLL coefficients calculate failed");
    bool need_config = true;
    portENTER_CRITICAL(&periph_spinlock);
    /* If APLL is not in use or only one peripheral in use, its frequency can be changed as will
     * But when more than one peripheral refers APLL, its frequency is not allowed to change once it is set */
    if (s_cur_apll_freq == 0 || s_apll_ref_cnt < 2) {
        s_cur_apll_freq = apll_freq;
    } else {
        apll_freq = s_cur_apll_freq;
        need_config = false;
    }
    portEXIT_CRITICAL(&periph_spinlock);
    *real_freq = apll_freq;

    if (need_config) {
        ESP_LOGD(TAG, "APLL will working at %"PRIu32" Hz with coefficients [sdm0] %"PRIu32" [sdm1] %"PRIu32" [sdm2] %"PRIu32" [o_div] %"PRIu32"",
                 apll_freq, sdm0, sdm1, sdm2, o_div);
        /* Set coefficients for APLL, notice that it doesn't mean APLL will start */
        rtc_clk_apll_coeff_set(o_div, sdm0, sdm1, sdm2);
    } else {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static uint32_t s_dac_set_apll_freq(uint32_t expt_freq)
{
    /* Set APLL coefficients to the given frequency */
    uint32_t real_freq = 0;
    esp_err_t ret = periph_rtc_apll_freq_set(expt_freq, &real_freq);
    if (ret == ESP_ERR_INVALID_ARG) {
        return 0;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "APLL is occupied already, it is working at %"PRIu32" Hz", real_freq);
    }
    ESP_LOGD(TAG, "APLL expected frequency is %"PRIu32" Hz, real frequency is %"PRIu32" Hz", expt_freq, real_freq);
    return real_freq;
}
/**
 * @brief Calculate and set DAC data frequency
 * @note  DAC clock shares clock divider with ADC, the clock source is APB or APLL on ESP32-S2
 *        freq_hz = (source_clk / (clk_div + (b / a) + 1)) / interval
 *        interval range: 1~4095
 * @param freq_hz    DAC byte transmit frequency
 * @return
 *      - ESP_OK    config success
 *      - ESP_ERR_INVALID_ARG   invalid frequency
 */
esp_err_t dac_dma_periph_init(int freq_hz, bool is_apll)
{
    /* Step 1: Determine the digital clock source frequency */
    uint32_t digi_ctrl_freq; // Digital controller clock
    if (is_apll) {
        /* Theoretical frequency range (due to the limitation of DAC, the maximum frequency may not reach):
         * CLK_LL_APLL_MAX_HZ: 119.24 Hz ~ 67.5 MHz
         * CLK_LL_APLL_MIN_HZ: 5.06 Hz ~ 2.65 MHz */
        digi_ctrl_freq = s_dac_set_apll_freq(freq_hz < 120 ? CLK_LL_APLL_MIN_HZ : CLK_LL_APLL_MAX_HZ);
        // ESP_RETURN_ON_FALSE(digi_ctrl_freq, ESP_ERR_INVALID_ARG, TAG, "set APLL coefficients failed");
    } else {
        digi_ctrl_freq = APB_CLK_FREQ;
    }

    /* Step 2: Determine the interval */
    uint32_t total_div = digi_ctrl_freq / freq_hz;
    uint32_t interval;
    /* For the case that smaller than the minimum ADC controller division, the required frequency is too big */
    // ESP_RETURN_ON_FALSE(total_div >= 2, ESP_ERR_INVALID_ARG, TAG, "the DAC frequency is too big");
    if (total_div < 256) { // For the case that smaller than the maximum ADC controller division
        /* Fix the interval to 1, the division is fully realized by the ADC controller clock divider */
        interval = 1;
    } else if (total_div < 8192) { // for the case that smaller than the maximum interval
        /* Set the interval to 'total_div / 2', fix the integer part of ADC controller clock division to 2 */
        interval = total_div / 2;
    } else {
        /* Fix the interval to 4095, */
        interval = 4095;
    }
    // ESP_RETURN_ON_FALSE(interval * 256 > total_div, ESP_ERR_INVALID_ARG, TAG, "the DAC frequency is too small");

    /* Step 3: Calculate the coefficients of ADC digital controller divider */
    hal_utils_clk_info_t adc_clk_info = {
        .src_freq_hz = digi_ctrl_freq / interval,
        .exp_freq_hz = freq_hz,
        .max_integ = 257,
        .min_integ = 1,
        .max_fract = 64,
    };
    hal_utils_clk_div_t adc_clk_div = {};
    hal_utils_calc_clk_div_frac_accurate(&adc_clk_info, &adc_clk_div);

    /* Step 4: Set the clock coefficients */
    dac_ll_digi_clk_inv(true);
    dac_ll_digi_set_trigger_interval(interval); // secondary clock division
    adc_ll_digi_controller_clk_div(adc_clk_div.integer - 1, adc_clk_div.denominator, adc_clk_div.numerator);
    // adc_ll_digi_clk_sel(is_apll ? ADC_DIGI_CLK_SRC_APLL : ADC_DIGI_CLK_SRC_DEFAULT);
    return ESP_OK;    
}
#endif
extern "C"
void IRAM_ATTR video_isr(const volatile void* buf);

// simple isr
void IRAM_ATTR i2s_intr_handler_video(void *arg)
{
#if CONFIG_IDF_TARGET_ESP32S2
    if (GPSPI3.dma_int_st.out_eof)
        video_isr(((lldesc_t*)GPSPI3.dma_out_eof_des_addr)->buf); // get the next line of video
    GPSPI3.dma_int_clr.val = GPSPI3.dma_int_st.val;
#else
    if (I2S0.int_st.out_eof)
        video_isr(((lldesc_t*)I2S0.out_eof_des_addr)->buf); // get the next line of video
    I2S0.int_clr.val = I2S0.int_st.val;                     // reset the interrupt
#endif
}

/**
 * @brief Power up APLL circuit
 */
static inline __attribute__((always_inline)) void clk_ll_apll_enable(void)
{
    CLEAR_PERI_REG_MASK(RTC_CNTL_ANA_CONF_REG, RTC_CNTL_PLLA_FORCE_PD);
    SET_PERI_REG_MASK(RTC_CNTL_ANA_CONF_REG, RTC_CNTL_PLLA_FORCE_PU);
}

// static inline __attribute__((always_inline)) void clk_ll_apll_set_config(uint32_t o_div, uint32_t sdm0, uint32_t sdm1, uint32_t sdm2)
// {
//     REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_DSDM2, sdm2);
//     REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_DSDM0, sdm0);
//     REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_DSDM1, sdm1);
//     REGI2C_WRITE(I2C_APLL, I2C_APLL_SDM_STOP, CLK_LL_APLL_SDM_STOP_VAL_1);
//     REGI2C_WRITE(I2C_APLL, I2C_APLL_SDM_STOP, CLK_LL_APLL_SDM_STOP_VAL_2_REV1);
//     REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_OR_OUTPUT_DIV, o_div);
// }

static esp_err_t start_dma(int line_width,int samples_per_cc, int ch = 1)
{
    // rtc_clk_config_t rclk = RTC_CLK_CONFIG_DEFAULT();
    // // rclk.xtal_freq = 20;
    // rclk.cpu_freq_mhz = 240;
    // // rclk.fast_freq = RTC_FAST_FREQ_XTALD4;
    // rtc_clk_init(rclk);
    setCpuFrequencyMhz(240);

#ifdef CONFIG_IDF_TARGET_ESP32S2
    uint32_t int_mask = SPI_OUT_EOF_INT_ENA;
    periph_module_enable(PERIPH_SPI3_DMA_MODULE);
    periph_module_enable(PERIPH_SARADC_MODULE);
    REG_SET_BIT(DPORT_PERIP_CLK_EN_REG, DPORT_APB_SARADC_CLK_EN_M);
    REG_SET_BIT(DPORT_PERIP_CLK_EN_REG, DPORT_SPI3_DMA_CLK_EN_M);
    REG_SET_BIT(DPORT_PERIP_CLK_EN_REG, DPORT_SPI3_CLK_EN);
    REG_CLR_BIT(DPORT_PERIP_RST_EN_REG, DPORT_APB_SARADC_RST_M);
    REG_CLR_BIT(DPORT_PERIP_RST_EN_REG, DPORT_SPI3_DMA_RST_M);
    REG_CLR_BIT(DPORT_PERIP_RST_EN_REG, DPORT_SPI3_RST_M);
    // REG_WRITE(SPI_DMA_INT_CLR_REG(3), 0xFFFFFFFF);
    REG_WRITE(SPI_DMA_INT_ENA_REG(3), int_mask | REG_READ(SPI_DMA_INT_ENA_REG(3)));
    REG_SET_BIT(SPI_DMA_OUT_LINK_REG(3), SPI_OUTLINK_STOP);
    REG_CLR_BIT(SPI_DMA_OUT_LINK_REG(3), SPI_OUTLINK_START);
    adc_ll_digi_clk_sel(true);
    dac_output_enable(DAC_CHANNEL_1);
    /* Acquire DMA peripheral */
    // dac_output_enable(DAC_CHANNEL_2);
    // GPSPI3.cmd.val = 1;
    // GPSPI3.dma_conf.out_eof_mode = 1;
    // GPSPI3.dma_int_ena.out_eof = 1;
    // Create TX DMA buffers
    for (int i = 0; i < 2; i++) {
        int n = line_width*ch*2;
        if (n >= 4092) {
            printf("DMA chunk too big:%d\n",n);
            return -1;
        }
        _dma_desc[i].buf = (uint8_t*)heap_caps_calloc(1, n, MALLOC_CAP_DMA);
        if (!_dma_desc[i].buf)
            return -1;

        _dma_desc[i].owner = 1;
        _dma_desc[i].eof = 1;
        _dma_desc[i].length = n;
        _dma_desc[i].size = n;
        _dma_desc[i].empty = (uint32_t)(i == 1 ? _dma_desc : _dma_desc+1);
    }
    // GPSPI3.dma_conf.out_eof_mode = 0;
    // GPSPI3.dma_out_link.addr = (uint32_t)_dma_desc;
    // GPSPI3.dma_int_clr.val = 0xFFFFFFFF;
    // GPSPI3.dma_int_ena.out_eof = 1;
    REG_SET_BIT(SPI_DMA_CONF_REG(3), SPI_OUT_RST | SPI_AHBM_FIFO_RST | SPI_AHBM_RST);
    REG_CLR_BIT(SPI_DMA_CONF_REG(3), SPI_OUT_RST | SPI_AHBM_FIFO_RST | SPI_AHBM_RST);
    SET_PERI_REG_BITS(SPI_DMA_OUT_LINK_REG(3), SPI_OUTLINK_ADDR, (uint32_t)_dma_desc, 0);
    // REG_CLR_BIT(SPI_DMA_OUT_LINK_REG(3), SPI_OUTLINK_STOP);
    // REG_SET_BIT(SPI_DMA_OUT_LINK_REG(3), SPI_OUTLINK_START);    
    // spi_ll_enable_bus_clock(SPI3_HOST, true);
    dac_ll_power_on(DAC_CHANNEL_1);
    // spi_ll_master_init(&GPSPI3);
    // spi_ll_enable_intr(&GPSPI3, (spi_ll_intr_t) (SPI_LL_INTR_TRANS_DONE));
    // dac_ll_digi_set_convert_mode(DAC_CONV_NORMAL);
    dac_ll_rtc_reset(); 
    // adc_ll_digi_controller_clk_div(0, 0, 0);

    //  Setup up the apll: See ref 3.2.7 Audio PLL
    //  f_xtal = (int)rtc_clk_xtal_freq_get() * 1000000;
    //  f_out = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536); // 250 < f_out < 500
    //  apll_freq = f_out/((o_div + 2) * 2)
    //  operating range of the f_out is 250 MHz ~ 500 MHz
    //  operating range of the apll_freq is 16 ~ 128 MHz.
    //  select sdm0,sdm1,sdm2 to produce nice multiples of colorburst frequencies

    //  see calc_freq() for math: (4+a)*10/((2 + b)*2) mhz
    //  up to 20mhz seems to work ok:
    //  rtc_clk_apll_enable(1,0x00,0x00,0x4,0);   // 20mhz for fancy DDS
    // rtc_clk_apll_enable(true)
    clk_ll_apll_enable();
    adc_ll_digi_dma_enable();
    if (!_pal_) {
        switch (samples_per_cc) {
            case 3:  rtc_clk_apll_coeff_set(2, 0x46,0x97,0x4);   break;    // 10.7386363636 3x NTSC (10.7386398315mhz)
            case 4:  rtc_clk_apll_coeff_set(5, 0x46,0x97,0x4);   break;    // 14.3181818182 4x NTSC (14.3181864421mhz)
        }
    } else {
         rtc_clk_apll_coeff_set(1, 0x04,0xA4,0x6);     // 17.734476mhz ~4x PAL
    }
    // dac_ll_digi_clk_inv(true);
    // *portOutputRegister(0)=1;
    // int a = *portInputRegister(0);
    // dac_ll_digi_set_trigger_interval(0);
    // rtc_clk_8m_enable(true, true);
    // dac_digi_config_t conf;
    // conf.mode = DAC_CONV_NORMAL;
    // conf.interval = 0;
    // adc_digi_clk_t adclk;
    // // Set ADC digital controller clock division factor. The clock divided from `APLL` or `APB` clock.
    // // Expression: controller_clk = (APLL or APB) / (div_num + div_a / div_b + 1).
    // adclk.use_apll = true;
    // adclk.div_num = 1;
    // adclk.div_a = 0;
    // adclk.div_b = 1;
    // conf.dig_clk = adclk;
    // dac_hal_digi_controller_config(&conf);

    // s_dac_set_apll_freq(14.3181818182 * 1000000);
    // dac_dma_periph_init(14.3181818182 * 1000000, true);
    // dac_ll_digi_set_convert_mode(conf.mode);
    // adc_ll_digi_controller_clk_div(4, 4, 0);
    // dac_dma_periph_init()
    // dac_hal_rtc_sync_by_adc(true);
    spi_dma_ll_tx_enable_burst_data(&GPSPI3, 1, true);
    spi_dma_ll_tx_enable_burst_desc(&GPSPI3, 1, true);
    spi_dma_ll_set_out_eof_generation(&GPSPI3, 1, true);
    spi_dma_ll_enable_out_auto_wrback(&GPSPI3, 1, true);
    spi_dma_ll_tx_start(&GPSPI3, 1, (lldesc_t *)_dma_desc);
    // if (!_pal_) {
    //     switch (samples_per_cc) {
    //         case 3: 
    //             // adc_ll_digi_controller_clk_div(5, 63, 43);
    //             // rtc_clk_apll_enable(false,13,181,255,31);   
    //             // dac_dma_periph_init(10738636, true);
    //             rtc_clk_apll_enable(1,0x46,0x97,0x4,2);  
    //             break;    // 10.7386363636 3x NTSC (10.7386398315mhz)
    //         case 4: 
    //             // rtc_clk_apll_enable(true,120,160,0,31);   
    //             // dac_dma_periph_init(14318181, true);
    //             rtc_clk_apll_enable(1,0x46,0x97,0x4,1);  
    //             break;    // 14.3181818182 4x NTSC (14.3181864421mhz)
    //     }
    // } else {
    //     rtc_clk_apll_enable(1,0x04,0xA4,0x6,1);     // 17.734476mhz ~4x PAL
    //     // rtc_clk_apll_enable(false,25,167,10,31);     // 17.734476mhz ~4x PAL
    //     // dac_dma_periph_init(17734476, true);
    //     // rtc_clk_apll_enable(1,0x043,0xA4,0x6,1);     // 17.734476mhz ~4x PAL
    //     // adc_ll_digi_controller_clk_div(0, 10, 1);
    // }
    // dac_hal_digi_enable_dma(true);
    // dac_ll_digi_set_trigger_interval(1);
    // adc_ll_digi_controller_clk_div(1, 0, 1);
    // rtc_clk_apb_freq_update(250000000);
    // APB_SARADC.apb_adc_clkm_conf.clk_en = true;
    // dac_ll_digi_trigger_output(true);
    // APB_SARADC.apb_dac_ctrl.dac_timer_target = 2;
    // APB_SARADC.apb_dac_ctrl.dac_timer_en = true;
    // APB_SARADC.apb_adc_arb_ctrl.adc_arb_apb_force = true;
    // APB_SARADC.apb_adc_arb_ctrl.adc_arb_grant_force = true;
    // APB_SARADC.apb_adc_arb_ctrl.adc_arb_apb_priority = 1;
    if (esp_intr_alloc(ETS_SPI3_DMA_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM,
        i2s_intr_handler_video, 0, &_isr_handle) != ESP_OK)
        return -1;
    dac_digi_start();

    // GPSPI3.dma_int_clr.val = -1;
    // GPSPI3.dma_int_ena.out_eof = 1;
    // GPSPI3.dma_out_link.dma_tx_ena = 1;                     // start DMA!
    // GPSPI3.dma_out_link.start = 1;
    // GPSPI3.dma_conf.ahbm_rst = 1;
    // GPSPI3.dma_conf.ahbm_fifo_rst = 1;
    // GPSPI3.dma_conf.ahbm_rst = 0;
    // GPSPI3.dma_conf.ahbm_fifo_rst = 0;
    // dac_ll_rtc_reset();
    // dac_ll_rtc_sync_by_adc(true);
    // dac_ll_digi_clk_inv(true);
    // spi_dma_ll_tx_reset(&GPSPI3, 1);
    // spi_dma_ll_tx_reset(&GPSPI3, 0);
    // spi_ll_dma_tx_fifo_reset(&GPSPI3);
    // spi_ll_dma_tx_enable(&GPSPI3, true);


#elif defined (CONFIG_IDF_TARGET_ESP32)
    periph_module_enable(PERIPH_I2S0_MODULE);

    // setup interrupt
    if (esp_intr_alloc(ETS_I2S0_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM,
        i2s_intr_handler_video, 0, &_isr_handle) != ESP_OK)
        return -1;

    // reset conf
    I2S0.conf.val = 1;
    I2S0.conf.val = 0;
    I2S0.conf.tx_right_first = 1;
    I2S0.conf.tx_mono = (ch == 2 ? 0 : 1);

    I2S0.conf2.lcd_en = 1;
    I2S0.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S0.sample_rate_conf.tx_bits_mod = 16;
    I2S0.conf_chan.tx_chan_mod = (ch == 2) ? 0 : 1;

    // Create TX DMA buffers
    for (int i = 0; i < 2; i++) {
        int n = line_width*2*ch;
        if (n >= 4092) {
            printf("DMA chunk too big:%d\n",n);
            return -1;
        }
        _dma_desc[i].buf = (uint8_t*)heap_caps_calloc(1, n, MALLOC_CAP_DMA);
        if (!_dma_desc[i].buf)
            return -1;

        _dma_desc[i].owner = 1;
        _dma_desc[i].eof = 1;
        _dma_desc[i].length = n;
        _dma_desc[i].size = n;
        _dma_desc[i].empty = (uint32_t)(i == 1 ? _dma_desc : _dma_desc+1);
    }
    I2S0.out_link.addr = (uint32_t)_dma_desc;

    //  Setup up the apll: See ref 3.2.7 Audio PLL
    //  f_xtal = (int)rtc_clk_xtal_freq_get() * 1000000;
    //  f_out = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536); // 250 < f_out < 500
    //  apll_freq = f_out/((o_div + 2) * 2)
    //  operating range of the f_out is 250 MHz ~ 500 MHz
    //  operating range of the apll_freq is 16 ~ 128 MHz.
    //  select sdm0,sdm1,sdm2 to produce nice multiples of colorburst frequencies

    //  see calc_freq() for math: (4+a)*10/((2 + b)*2) mhz
    //  up to 20mhz seems to work ok:
    //  rtc_clk_apll_enable(1,0x00,0x00,0x4,0);   // 20mhz for fancy DDS

    if (!_pal_) {
        switch (samples_per_cc) {
            case 3: rtc_clk_apll_enable(1,0x46,0x97,0x4,2);   break;    // 10.7386363636 3x NTSC (10.7386398315mhz)
            case 4: rtc_clk_apll_enable(1,0x46,0x97,0x4,1);   break;    // 14.3181818182 4x NTSC (14.3181864421mhz)
        }
    } else {
        rtc_clk_apll_enable(1,0x04,0xA4,0x6,1);     // 17.734476mhz ~4x PAL
    }
    I2S0.clkm_conf.clkm_div_num = 1;            // I2S clock divider’s integral value.
    I2S0.clkm_conf.clkm_div_b = 0;              // Fractional clock divider’s numerator value.
    I2S0.clkm_conf.clkm_div_a = 1;              // Fractional clock divider’s denominator value
    I2S0.sample_rate_conf.tx_bck_div_num = 1;
    I2S0.clkm_conf.clka_en = 1;                 // Set this bit to enable clk_apll.
    I2S0.fifo_conf.tx_fifo_mod = (ch == 2) ? 0 : 1; // 32-bit dual or 16-bit single channel data

    dac_output_enable(DAC_CHANNEL_1);           // DAC, video on GPIO25
    dac_i2s_enable();                           // start DAC!

    I2S0.conf.tx_start = 1;                     // start DMA!
    I2S0.int_clr.val = 0xFFFFFFFF;
    I2S0.int_ena.out_eof = 1;
    I2S0.out_link.start = 1;
#endif 
    return esp_intr_enable(_isr_handle);        // start interruprs!
}

void video_init_hw(int line_width, int samples_per_cc)
{
    // setup apll 4x NTSC or PAL colorburst rate
    start_dma(line_width,samples_per_cc,1);

    // Now ideally we would like to use the decoupled left DAC channel to produce audio
    // But when using the APLL there appears to be some clock domain conflict that causes
    // nasty digitial spikes and dropouts.
}

//====================================================================================================
//====================================================================================================


// ntsc phase representation of a rrrgggbb pixel
// must be in RAM so VBL works
const static DRAM_ATTR uint32_t ntsc_RGB332[256] = {
    0x18181818,0x18171A1C,0x1A151D22,0x1B141F26,0x1D1C1A1B,0x1E1B1C20,0x20191F26,0x2119222A,
    0x23201C1F,0x241F1E24,0x251E222A,0x261D242E,0x29241F23,0x2A232128,0x2B22242E,0x2C212632,
    0x2E282127,0x2F27232C,0x31262732,0x32252936,0x342C232B,0x352B2630,0x372A2936,0x38292B3A,
    0x3A30262F,0x3B2F2833,0x3C2E2B3A,0x3D2D2E3E,0x40352834,0x41342B38,0x43332E3E,0x44323042,
    0x181B1B18,0x191A1D1C,0x1B192022,0x1C182327,0x1E1F1D1C,0x1F1E2020,0x201D2326,0x211C252B,
    0x24232020,0x25222224,0x2621252A,0x2720272F,0x29272224,0x2A262428,0x2C25282E,0x2D242A33,
    0x2F2B2428,0x302A272C,0x32292A32,0x33282C37,0x352F272C,0x362E2930,0x372D2C36,0x382C2F3B,
    0x3B332930,0x3C332B34,0x3D312F3A,0x3E30313F,0x41382C35,0x42372E39,0x4336313F,0x44353443,
    0x191E1E19,0x1A1D211D,0x1B1C2423,0x1C1B2628,0x1F22211D,0x20212321,0x21202627,0x221F292C,
    0x24262321,0x25252525,0x2724292B,0x28232B30,0x2A2A2625,0x2B292829,0x2D282B2F,0x2E272D34,
    0x302E2829,0x312E2A2D,0x322C2D33,0x332B3038,0x36332A2D,0x37322C31,0x38303037,0x392F323C,
    0x3B372D31,0x3C362F35,0x3E35323B,0x3F343440,0x423B2F36,0x423A313A,0x44393540,0x45383744,
    0x1A21221A,0x1B20241E,0x1C1F2724,0x1D1E2A29,0x1F25241E,0x20242622,0x22232A28,0x23222C2D,
    0x25292722,0x26292926,0x27272C2C,0x28262E30,0x2B2E2926,0x2C2D2B2A,0x2D2B2E30,0x2E2A3134,
    0x31322B2A,0x32312E2E,0x332F3134,0x342F3338,0x36362E2E,0x37353032,0x39343338,0x3A33363C,
    0x3C3A3032,0x3D393236,0x3E38363C,0x3F373840,0x423E3337,0x433E353B,0x453C3841,0x463B3A45,
    0x1A24251B,0x1B24271F,0x1D222B25,0x1E212D29,0x2029281F,0x21282A23,0x22262D29,0x23252F2D,
    0x262D2A23,0x272C2C27,0x282A2F2D,0x292A3231,0x2C312C27,0x2C302F2B,0x2E2F3231,0x2F2E3435,
    0x31352F2B,0x3234312F,0x34333435,0x35323739,0x3739312F,0x38383333,0x39373739,0x3A36393D,
    0x3D3D3433,0x3E3C3637,0x3F3B393D,0x403A3B41,0x43423637,0x4441383B,0x453F3C42,0x463F3E46,
    0x1B28291C,0x1C272B20,0x1D252E26,0x1E25302A,0x212C2B20,0x222B2D24,0x232A312A,0x2429332E,
    0x26302D24,0x272F3028,0x292E332E,0x2A2D3532,0x2C343028,0x2D33322C,0x2F323532,0x30313836,
    0x3238322C,0x33373430,0x34363836,0x35353A3A,0x383C3530,0x393B3734,0x3A3A3A3A,0x3B393C3E,
    0x3D403734,0x3E403938,0x403E3C3E,0x413D3F42,0x44453A38,0x45443C3C,0x46433F42,0x47424147,
    0x1C2B2C1D,0x1D2A2E21,0x1E293227,0x1F28342B,0x212F2E21,0x222E3125,0x242D342B,0x252C362F,
    0x27333125,0x28323329,0x2A31362F,0x2B303933,0x2D373329,0x2E36352D,0x2F353933,0x30343B37,
    0x333B362D,0x343B3831,0x35393B37,0x36383D3B,0x38403831,0x393F3A35,0x3B3D3E3B,0x3C3C403F,
    0x3E443A35,0x3F433D39,0x4141403F,0x42414243,0x44483D39,0x45473F3D,0x47464243,0x48454548,
    0x1C2E301E,0x1D2E3222,0x1F2C3528,0x202B382C,0x22333222,0x23323426,0x2530382C,0x262F3A30,
    0x28373526,0x2936372A,0x2A343A30,0x2B343C34,0x2E3B372A,0x2F3A392E,0x30393C34,0x31383F38,
    0x333F392E,0x343E3C32,0x363D3F38,0x373C413C,0x39433C32,0x3A423E36,0x3C41413C,0x3D404440,
    0x3F473E36,0x4046403A,0x41454440,0x42444644,0x454C413A,0x464B433E,0x47494644,0x49494949,
};

// PAL yuyv palette, must be in RAM
const static DRAM_ATTR uint32_t pal_yuyv[] = {
    0x18181818,0x1A16191E,0x1E121A26,0x21101A2C,0x1E1D1A1B,0x211B1A20,0x25171B29,0x27151C2E,
    0x25231B1E,0x27201C23,0x2B1D1D2B,0x2E1A1E31,0x2B281D20,0x2E261E26,0x31221F2E,0x34202034,
    0x322D1F23,0x342B2029,0x38282131,0x3A252137,0x38332126,0x3A30212B,0x3E2D2234,0x412A2339,
    0x3E382229,0x4136232E,0x44322436,0x4730253C,0x453E242C,0x483C2531,0x4B382639,0x4E36273F,
    0x171B1D19,0x1A181E1F,0x1D151F27,0x20121F2D,0x1E201F1C,0x201E1F22,0x241A202A,0x26182130,
    0x2425201F,0x27232124,0x2A20222D,0x2D1D2332,0x2A2B2222,0x2D282327,0x3125242F,0x33222435,
    0x31302424,0x332E242A,0x372A2632,0x3A282638,0x37362627,0x3A33262D,0x3D302735,0x402D283B,
    0x3E3B272A,0x4039282F,0x44352938,0x46332A3D,0x4441292D,0x473E2A32,0x4B3B2B3B,0x4D382C40,
    0x171D221B,0x191B2220,0x1D182329,0x1F15242E,0x1D23231E,0x1F202423,0x231D252B,0x261A2631,
    0x23282520,0x26262626,0x2A22272E,0x2C202834,0x2A2E2723,0x2C2B2829,0x30282931,0x33252937,
    0x30332926,0x3331292B,0x362D2A34,0x392B2B39,0x36382A29,0x39362B2E,0x3D322C36,0x3F302D3C,
    0x3D3E2C2B,0x3F3B2D31,0x43382E39,0x46352F3F,0x44432E2E,0x46412F34,0x4A3E303C,0x4D3B3042,
    0x1620271C,0x181E2722,0x1C1A282A,0x1F182930,0x1C26281F,0x1F232924,0x22202A2D,0x251D2B32,
    0x232B2A22,0x25292B27,0x29252C30,0x2B232C35,0x29302C24,0x2C2E2C2A,0x2F2A2D32,0x32282E38,
    0x2F362D27,0x32332E2D,0x36302F35,0x382D303B,0x363B2F2A,0x38393030,0x3C353138,0x3F33323E,
    0x3C40312D,0x3F3E3232,0x423A333B,0x45383340,0x43463330,0x46443435,0x4940353E,0x4C3E3543,
    0x15232B1E,0x18212C23,0x1B1D2D2B,0x1E1B2E31,0x1C282D20,0x1E262E26,0x22222F2E,0x24202F34,
    0x222E2F23,0x242B3029,0x28283131,0x2B253137,0x28333126,0x2B31312B,0x2F2D3234,0x312B3339,
    0x2F383229,0x3136332E,0x35323436,0x3730353C,0x353E342B,0x383B3531,0x3B383639,0x3E35363F,
    0x3B43362E,0x3E413634,0x423D373C,0x443B3842,0x42493831,0x45473837,0x4943393F,0x4B413A45,
    0x1526301F,0x17233125,0x1B20322D,0x1D1D3333,0x1B2B3222,0x1D293327,0x21253430,0x24233435,
    0x21303425,0x242E342A,0x272A3532,0x2A283638,0x28363527,0x2A33362D,0x2E303735,0x302D383B,
    0x2E3B372A,0x30393830,0x34353938,0x37333A3E,0x3440392D,0x373E3A32,0x3B3B3B3B,0x3D383B40,
    0x3B463B30,0x3D433B35,0x41403C3D,0x443D3D43,0x424C3D33,0x44493D38,0x48463E40,0x4A433F46,
    0x14283520,0x16263626,0x1A23372E,0x1D203734,0x1A2E3723,0x1D2B3729,0x20283831,0x23253937,
    0x21333826,0x2331392B,0x272D3A34,0x292B3B39,0x27383A29,0x29363B2E,0x2D333C36,0x30303D3C,
    0x2D3E3C2B,0x303B3D31,0x34383E39,0x36363E3F,0x34433E2E,0x36413E34,0x3A3D3F3C,0x3C3B4042,
    0x3A493F31,0x3D464036,0x4043413F,0x43404244,0x414E4134,0x434C4239,0x47484342,0x4A464447,
    0x132B3A22,0x16293B27,0x19253C30,0x1C233D35,0x19313C25,0x1C2E3D2A,0x202B3E32,0x22283E38,
    0x20363E27,0x22343E2D,0x26303F35,0x292E403B,0x263B3F2A,0x29394030,0x2C364138,0x2F33423E,
    0x2D41412D,0x2F3E4232,0x333B433B,0x35384440,0x33464330,0x35444435,0x3940453E,0x3C3E4543,
    0x394C4533,0x3C494538,0x40464640,0x42434746,0x40514735,0x434F473B,0x464B4843,0x49494949,
    //odd
    0x18181818,0x19161A1E,0x1A121E26,0x1A10212C,0x1A1D1E1B,0x1A1B2120,0x1B172529,0x1C15272E,
    0x1B23251E,0x1C202723,0x1D1D2B2B,0x1E1A2E31,0x1D282B20,0x1E262E26,0x1F22312E,0x20203434,
    0x1F2D3223,0x202B3429,0x21283831,0x21253A37,0x21333826,0x21303A2B,0x222D3E34,0x232A4139,
    0x22383E29,0x2336412E,0x24324436,0x2530473C,0x243E452C,0x253C4831,0x26384B39,0x27364E3F,
    0x1D1B1719,0x1E181A1F,0x1F151D27,0x1F12202D,0x1F201E1C,0x1F1E2022,0x201A242A,0x21182630,
    0x2025241F,0x21232724,0x22202A2D,0x231D2D32,0x222B2A22,0x23282D27,0x2425312F,0x24223335,
    0x24303124,0x242E332A,0x262A3732,0x26283A38,0x26363727,0x26333A2D,0x27303D35,0x282D403B,
    0x273B3E2A,0x2839402F,0x29354438,0x2A33463D,0x2941442D,0x2A3E4732,0x2B3B4B3B,0x2C384D40,
    0x221D171B,0x221B1920,0x23181D29,0x24151F2E,0x23231D1E,0x24201F23,0x251D232B,0x261A2631,
    0x25282320,0x26262626,0x27222A2E,0x28202C34,0x272E2A23,0x282B2C29,0x29283031,0x29253337,
    0x29333026,0x2931332B,0x2A2D3634,0x2B2B3939,0x2A383629,0x2B36392E,0x2C323D36,0x2D303F3C,
    0x2C3E3D2B,0x2D3B3F31,0x2E384339,0x2F35463F,0x2E43442E,0x2F414634,0x303E4A3C,0x303B4D42,
    0x2720161C,0x271E1822,0x281A1C2A,0x29181F30,0x28261C1F,0x29231F24,0x2A20222D,0x2B1D2532,
    0x2A2B2322,0x2B292527,0x2C252930,0x2C232B35,0x2C302924,0x2C2E2C2A,0x2D2A2F32,0x2E283238,
    0x2D362F27,0x2E33322D,0x2F303635,0x302D383B,0x2F3B362A,0x30393830,0x31353C38,0x32333F3E,
    0x31403C2D,0x323E3F32,0x333A423B,0x33384540,0x33464330,0x34444635,0x3540493E,0x353E4C43,
    0x2B23151E,0x2C211823,0x2D1D1B2B,0x2E1B1E31,0x2D281C20,0x2E261E26,0x2F22222E,0x2F202434,
    0x2F2E2223,0x302B2429,0x31282831,0x31252B37,0x31332826,0x31312B2B,0x322D2F34,0x332B3139,
    0x32382F29,0x3336312E,0x34323536,0x3530373C,0x343E352B,0x353B3831,0x36383B39,0x36353E3F,
    0x36433B2E,0x36413E34,0x373D423C,0x383B4442,0x38494231,0x38474537,0x3943493F,0x3A414B45,
    0x3026151F,0x31231725,0x32201B2D,0x331D1D33,0x322B1B22,0x33291D27,0x34252130,0x34232435,
    0x34302125,0x342E242A,0x352A2732,0x36282A38,0x35362827,0x36332A2D,0x37302E35,0x382D303B,
    0x373B2E2A,0x38393030,0x39353438,0x3A33373E,0x3940342D,0x3A3E3732,0x3B3B3B3B,0x3B383D40,
    0x3B463B30,0x3B433D35,0x3C40413D,0x3D3D4443,0x3D4C4233,0x3D494438,0x3E464840,0x3F434A46,
    0x35281420,0x36261626,0x37231A2E,0x37201D34,0x372E1A23,0x372B1D29,0x38282031,0x39252337,
    0x38332126,0x3931232B,0x3A2D2734,0x3B2B2939,0x3A382729,0x3B36292E,0x3C332D36,0x3D30303C,
    0x3C3E2D2B,0x3D3B3031,0x3E383439,0x3E36363F,0x3E43342E,0x3E413634,0x3F3D3A3C,0x403B3C42,
    0x3F493A31,0x40463D36,0x4143403F,0x42404344,0x414E4134,0x424C4339,0x43484742,0x44464A47,
    0x3A2B1322,0x3B291627,0x3C251930,0x3D231C35,0x3C311925,0x3D2E1C2A,0x3E2B2032,0x3E282238,
    0x3E362027,0x3E34222D,0x3F302635,0x402E293B,0x3F3B262A,0x40392930,0x41362C38,0x42332F3E,
    0x41412D2D,0x423E2F32,0x433B333B,0x44383540,0x43463330,0x44443535,0x4540393E,0x453E3C43,
    0x454C3933,0x45493C38,0x46464040,0x47434246,0x47514035,0x474F433B,0x484B4643,0x49494949,
};

//====================================================================================================
//====================================================================================================

uint32_t cpu_ticks()
{
  return xthal_get_ccount();
}

uint32_t us() {
    return cpu_ticks()/240;
}

// Color clock frequency is 315/88 (3.57954545455)
// DAC_MHZ is 315/11 or 8x color clock
// 455/2 color clocks per line, round up to maintain phase
// HSYNCH period is 44/315*455 or 63.55555..us
// Field period is 262*44/315*455 or 16651.5555us

#define IRE(_x)          ((uint32_t)(((_x)+40)*255/3.3/147.5) << 8)   // 3.3V DAC
#define SYNC_LEVEL       IRE(-40)
#define BLANKING_LEVEL   IRE(0)
#define BLACK_LEVEL      IRE(7.5)
#define GRAY_LEVEL       IRE(50)
#define WHITE_LEVEL      IRE(100)


#define P0 (color >> 16)
#define P1 (color >> 8)
#define P2 (color)
#define P3 (color << 8)

// Double-buffering: _bufferA and _bufferB will be swapped back and forth.
static uint8_t** _bufferA;
static uint8_t** _bufferB;

// _lines may point to either _bufferA or _bufferB, depending on which is being displayed
// _backBuffer points to whichever one _lines is not pointing to
static uint8_t** _lines; // Front buffer currently on display
static uint8_t** _backBuffer; // Back buffer waiting to be swapped to front

// TRUE when _backBuffer is ready to go.
static bool _swapReady;

// Notification handle once front and back buffers have been swapped.
static TaskHandle_t _swapCompleteNotify;

// Number of swaps completed
static uint32_t _swap_counter = 0;

volatile int _line_counter = 0;
volatile uint32_t _frame_counter = 0;

int _active_lines;
int _line_count;

int _line_width;
int _samples_per_cc;
const uint32_t* _palette;

float _sample_rate;

int _hsync;
int _hsync_long;
int _hsync_short;
int _burst_start;
int _burst_width;
int _active_start;

int16_t* _burst0 = 0; // pal bursts
int16_t* _burst1 = 0;

static int usec(float us)
{
    uint32_t r = (uint32_t)(us*_sample_rate);
    return ((r + _samples_per_cc)/(_samples_per_cc << 1))*(_samples_per_cc << 1);  // multiple of color clock, word align
}

#define NTSC_COLOR_CLOCKS_PER_SCANLINE 228       // really 227.5 for NTSC but want to avoid half phase fiddling for now
#define NTSC_FREQUENCY (315000000.0/88)
#define NTSC_LINES 262

#define PAL_COLOR_CLOCKS_PER_SCANLINE 284        // really 283.75 ?
#define PAL_FREQUENCY 4433618.75
#define PAL_LINES 312

void pal_init();

void video_init(int samples_per_cc, int ntsc)
{
    _samples_per_cc = samples_per_cc;

    if (ntsc) {
        _sample_rate = 315.0/88 * samples_per_cc;   // DAC rate
        _line_width = NTSC_COLOR_CLOCKS_PER_SCANLINE*samples_per_cc;
        _line_count = NTSC_LINES;
        _hsync_long = usec(63.555-4.7);
        _active_start = usec(samples_per_cc == 4 ? 10 : 10.5);
        _hsync = usec(4.7);
        _palette = ntsc_RGB332;
        _pal_ = 0;
    } else {
        pal_init();
        _palette = pal_yuyv;
        _pal_ = 1;
    }

    _active_lines = 240;
    video_init_hw(_line_width,_samples_per_cc);    // init the hardware
}

//===================================================================================================
//===================================================================================================
// PAL

void pal_init()
{
    int cc_width = 4;
    _sample_rate = PAL_FREQUENCY*cc_width/1000000.0;       // DAC rate in mhz
    _line_width = PAL_COLOR_CLOCKS_PER_SCANLINE*cc_width;
    _line_count = PAL_LINES;
    _hsync_short = usec(2);
    _hsync_long = usec(30);
    _hsync = usec(4.7);
    _burst_start = usec(5.6);
    _burst_width = (int)(10*cc_width + 4) & 0xFFFE;
    _active_start = usec(10.4);

    // make colorburst tables for even and odd lines
    _burst0 = new int16_t[_burst_width];
    _burst1 = new int16_t[_burst_width];
    float phase = 2*M_PI/2;
    for (int i = 0; i < _burst_width; i++)
    {
        _burst0[i] = BLANKING_LEVEL + sin(phase + 3*M_PI/4) * BLANKING_LEVEL/1.5;
        _burst1[i] = BLANKING_LEVEL + sin(phase - 3*M_PI/4) * BLANKING_LEVEL/1.5;
        phase += 2*M_PI/cc_width;
    }
}

void IRAM_ATTR blit_pal(uint8_t* src, uint16_t* dst)
{
    uint32_t c,color;
    bool even = _line_counter & 1;
    const uint32_t* p = even ? _palette : _palette + 256;
    int left = 0;
    int right = 256;
    uint8_t mask = 0xFF;

    // 192 of 288 color clocks wide: roughly correct aspect ratio
    dst += 88;

    // 4 pixels over 3 color clocks, 12 samples
    // do the blitting
    for (int i = left; i < right; i += 4) {
        c = *((uint32_t*)(src+i));
        color = p[c & mask];
        dst[0^1] = P0;
        dst[1^1] = P1;
        dst[2^1] = P2;
        color = p[(c >> 8) & mask];
        dst[3^1] = P3;
        dst[4^1] = P0;
        dst[5^1] = P1;
        color = p[(c >> 16) & mask];
        dst[6^1] = P2;
        dst[7^1] = P3;
        dst[8^1] = P0;
        color = p[(c >> 24) & mask];
        dst[9^1] = P1;
        dst[10^1] = P2;
        dst[11^1] = P3;
        dst += 12;
    }
}

void IRAM_ATTR burst_pal(uint16_t* line)
{
    line += _burst_start;
    int16_t* b = (_line_counter & 1) ? _burst0 : _burst1;
    for (int i = 0; i < _burst_width; i += 2) {
        line[i^1] = b[i];
        line[(i+1)^1] = b[i+1];
    }
}

//===================================================================================================
//===================================================================================================
// ntsc tables
// AA AA                // 2 pixels, 1 color clock - atari
// AA AB BB             // 3 pixels, 2 color clocks - nes
// AAA ABB BBC CCC      // 4 pixels, 3 color clocks - sms

// cc == 3 gives 684 samples per line, 3 samples per cc, 3 pixels for 2 cc
// cc == 4 gives 912 samples per line, 4 samples per cc, 2 pixels per cc

#ifdef PERF
#define BEGIN_TIMING()  uint32_t t = cpu_ticks()
#define END_TIMING() t = cpu_ticks() - t; _blit_ticks_min = min(_blit_ticks_min,t); _blit_ticks_max = max(_blit_ticks_max,t);
#define ISR_BEGIN() uint32_t t = cpu_ticks()
#define ISR_END() t = cpu_ticks() - t;_isr_us += (t+120)/240;
uint32_t _blit_ticks_min = 0;
uint32_t _blit_ticks_max = 0;
uint32_t _isr_us = 0;
#else
#define BEGIN_TIMING()
#define END_TIMING()
#define ISR_BEGIN()
#define ISR_END()
#endif

// draw a line of game in NTSC
void IRAM_ATTR blit(uint8_t* src, uint16_t* dst)
{
    const uint32_t* p = _palette;
    uint32_t color,c;
    uint32_t mask = 0xFF;
    int i;

    BEGIN_TIMING();
    if (_pal_) {
        blit_pal(src,dst);
        END_TIMING();
        return;
    }

    // AAA ABB BBC CCC
    // 4 pixels, 3 color clocks, 4 samples per cc
    // each pixel gets 3 samples, 192 color clocks wide
    for (i = 0; i < 256; i += 4) {
        c = *((uint32_t*)(src+i));
        color = p[c & mask];
        dst[0^1] = P0;
        dst[1^1] = P1;
        dst[2^1] = P2;
        color = p[(c >> 8) & mask];
        dst[3^1] = P3;
        dst[4^1] = P0;
        dst[5^1] = P1;
        color = p[(c >> 16) & mask];
        dst[6^1] = P2;
        dst[7^1] = P3;
        dst[8^1] = P0;
        color = p[(c >> 24) & mask];
        dst[9^1] = P1;
        dst[10^1] = P2;
        dst[11^1] = P3;
        dst += 12;
    }

    END_TIMING();
}

void IRAM_ATTR burst(uint16_t* line)
{
    if (_pal_) {
        burst_pal(line);
        return;
    }

    int i,phase;
    switch (_samples_per_cc) {
        case 4:
            // 4 samples per color clock
            for (i = _hsync; i < _hsync + (4*10); i += 4) {
                line[i+1] = BLANKING_LEVEL;
                line[i+0] = BLANKING_LEVEL + BLANKING_LEVEL/2;
                line[i+3] = BLANKING_LEVEL;
                line[i+2] = BLANKING_LEVEL - BLANKING_LEVEL/2;
            }
            break;
        case 3:
            // 3 samples per color clock
            phase = 0.866025*BLANKING_LEVEL/2;
            for (i = _hsync; i < _hsync + (3*10); i += 6) {
                line[i+1] = BLANKING_LEVEL;
                line[i+0] = BLANKING_LEVEL + phase;
                line[i+3] = BLANKING_LEVEL - phase;
                line[i+2] = BLANKING_LEVEL;
                line[i+5] = BLANKING_LEVEL + phase;
                line[i+4] = BLANKING_LEVEL - phase;
            }
            break;
    }
}

void IRAM_ATTR sync(uint16_t* line, int syncwidth)
{
    for (int i = 0; i < syncwidth; i++)
        line[i] = SYNC_LEVEL;
}

void IRAM_ATTR blanking(uint16_t* line, bool vbl)
{
    int syncwidth = vbl ? _hsync_long : _hsync;
    sync(line,syncwidth);
    for (int i = syncwidth; i < _line_width; i++)
        line[i] = BLANKING_LEVEL;
    if (!vbl)
        burst(line);    // no burst during vbl
}

// Fancy pal non-interlace
// http://martin.hinner.info/vga/pal.html
void IRAM_ATTR pal_sync2(uint16_t* line, int width, int swidth)
{
    swidth = swidth ? _hsync_long : _hsync_short;
    int i;
    for (i = 0; i < swidth; i++)
        line[i] = SYNC_LEVEL;
    for (; i < width; i++)
        line[i] = BLANKING_LEVEL;
}

uint8_t DRAM_ATTR _sync_type[8] = {0,0,0,3,3,2,0,0};
void IRAM_ATTR pal_sync(uint16_t* line, int i)
{
    uint8_t t = _sync_type[i-304];
    pal_sync2(line,_line_width/2, t & 2);
    pal_sync2(line+_line_width/2,_line_width/2, t & 1);
}

// Wait for front and back buffers to swap before starting drawing
void video_sync()
{
  if (!_lines)
    return;
  ulTaskNotifyTake(pdTRUE, 0);
}

// Workhorse ISR handles audio and video updates
extern "C"
void IRAM_ATTR video_isr(const volatile void* vbuf)
{
    if (!_lines)
        return;

    ISR_BEGIN();

    int i = _line_counter++;
    uint16_t* buf = (uint16_t*)vbuf;
    if (_pal_) {
        // pal
        if (i < 32) {
            blanking(buf,false);                // pre render/black 0-32
        } else if (i < _active_lines + 32) {    // active video 32-272
            sync(buf,_hsync);
            burst(buf);
            blit(_lines[i-32],buf + _active_start);
        } else if (i < 304) {                   // post render/black 272-304
            blanking(buf,false);
        } else {
            pal_sync(buf,i);                    // 8 lines of sync 304-312
        }
    } else {
        // ntsc
        if (i < _active_lines) {                // active video
            sync(buf,_hsync);
            burst(buf);
            blit(_lines[i],buf + _active_start);

        } else if (i < (_active_lines + 5)) {   // post render/black
            blanking(buf,false);

        } else if (i < (_active_lines + 8)) {   // vsync
            blanking(buf,true);

        } else {                                // pre render/black
            blanking(buf,false);
        }
    }

    if (_line_counter == _line_count) {
        _line_counter = 0;                      // frame is done
        _frame_counter++;

        // Is the back buffer ready to go?
        if (_swapReady) {
          // Swap front and back buffers
          if (_lines == _bufferA) {
            _lines = _bufferB;
            _backBuffer = _bufferA;
          } else {
            _lines = _bufferA;
            _backBuffer = _bufferB;
          }
          _swapReady = false;
          _swap_counter++;

          // Signal video_sync() swap has completed
            vTaskNotifyGiveFromISR(
                _swapCompleteNotify,
                NULL);
        }
    }

    ISR_END();
}

//===================================================================================================
//===================================================================================================
// Wrapper class

/*
 * @brief Constructor for ESP_8_BIT composite video wrapper class
 * @param ntsc True (or nonzero) for NTSC mode, False (or zero) for PAL mode
 */
ESP_8_BIT_composite::ESP_8_BIT_composite(int ntsc)
{
  _pal_ = !ntsc;
  if (NULL == _instance_)
  {
    _instance_ = this;
  }
  _started = false;
  _bufferA = NULL;
  _bufferB = NULL;
}

/*
 * @brief Destructor for ESP_8_BIT composite video wrapper class.
 */
ESP_8_BIT_composite::~ESP_8_BIT_composite()
{
  if(_bufferA)
  {
    frameBufferFree(_bufferA);
    _bufferA= NULL;
  }
  if(_bufferB)
  {
    frameBufferFree(_bufferB);
    _bufferB= NULL;
  }
  if (_started)
  {
    // Free resources by mirroring everything allocated in start_dma()
    esp_intr_disable(_isr_handle);
#if CONFIG_IDF_TARGET_ESP32S2
    dac_hal_digi_enable_dma(false);
    dac_digi_stop();
#else
    dac_i2s_disable();
#endif
    dac_output_disable(DAC_CHANNEL_1);
    if (!_pal_) {
        rtc_clk_apll_enable(false,0x46,0x97,0x4,1);
    } else {
        rtc_clk_apll_enable(false,0x04,0xA4,0x6,1);
    }
    for (int i = 0; i < 2; i++) {
      heap_caps_free((void*)(_dma_desc[i].buf));
      _dma_desc[i].buf = NULL;
    }
    // Missing: There doesn't seem to be a esp_intr_free() to go with esp_intr_alloc()?
    periph_module_disable(PERIPH_I2S0_MODULE);
    _started = false;
  }
  _lines = NULL;
  _backBuffer = NULL;
  _instance_ = NULL;
}

/*
 * @brief Check to ensure this instance is the first and only allowed instance
 */
void ESP_8_BIT_composite::instance_check()
{
  if (_instance_ != this)
  {
    ESP_LOGE(TAG, "Only one instance of ESP_8_BIT_composite class is allowed.");
    ESP_ERROR_CHECK(ESP_FAIL);
  }
}

/*
 * @brief Video subsystem setup: allocate frame buffer and start engine
 */
void ESP_8_BIT_composite::begin()
{
  instance_check();

  if (_started)
  {
    ESP_LOGE(TAG, "begin() is only allowed to be called once.");
    ESP_ERROR_CHECK(ESP_FAIL);
  }
  _started = true;

  _bufferA = frameBufferAlloc();
  _bufferB = frameBufferAlloc();

  _lines = _bufferA;
  _backBuffer = _bufferB;

  // Initialize double-buffering infrastructure
  _swapReady = false;
  _swapCompleteNotify = xTaskGetCurrentTaskHandle();

  // Start video signal generator
  video_init(4, !_pal_);
}

/////////////////////////////////////////////////////////////////////////////
//
//  Frame buffer memory allocation notes
//
// Architecture can tolerate each _line[i] being a separate chunk of memory
// but allocating in tiny 256 byte chunks is inefficient. (16 bytes of
// overhead per allocation.) On the opposite end, allocating the entire
// buffer at once (256*240 = 60kB) demands a large contiguous chunk of
// memory which might not exist if memory space is fragmented.
//
// Compromise: Allocate frame buffer in 4kB chunks. This means each
// frame buffer is made of 15 4kB chunks instead of a single 60kB chunk.
//
// 14 extra allocations * 16 byte overhead = 224 extra bytes, worth it.

const uint16_t linesPerFrame = 240;
const uint16_t bytesPerLine = 256;
const uint16_t linesPerChunk = 16;
const uint16_t chunkSize = bytesPerLine*linesPerChunk;
const uint16_t chunksPerFrame = 15;

/*
 * @brief Allocate memory for frame buffer
 */
uint8_t** ESP_8_BIT_composite::frameBufferAlloc()
{
  uint8_t** lineArray = NULL;
  uint8_t*  lineChunk = NULL;
  uint8_t*  lineStep  = NULL;

  lineArray = new uint8_t*[linesPerFrame];
  if ( NULL == lineArray )
  {
    ESP_LOGE(TAG, "Frame lines array allocation fail");
    ESP_ERROR_CHECK(ESP_FAIL);
  }

  for (uint8_t chunk = 0; chunk < chunksPerFrame; chunk++)
  {
    lineChunk = new uint8_t[chunkSize];
    if ( NULL == lineChunk )
    {
      ESP_LOGE(TAG, "Frame buffer chunk allocation fail");
      ESP_ERROR_CHECK(ESP_FAIL);
    }
    lineStep = lineChunk;
    for (uint8_t lineIndex = 0; lineIndex < linesPerChunk; lineIndex++)
    {
      lineArray[(chunk*linesPerChunk)+lineIndex] = lineStep;
      lineStep += bytesPerLine;
    }
  }

  return lineArray;
}

/*
 * @brief Free memory allocated by frameBufferAlloc();
 */
void ESP_8_BIT_composite::frameBufferFree(uint8_t** lineArray)
{
  for (uint8_t chunk = 0; chunk < chunksPerFrame; chunk++)
  {
    free(lineArray[chunk*linesPerChunk]);
  }
  free(lineArray);
}

/*
 * @brief Wait for current frame to finish rendering
 */
void ESP_8_BIT_composite::waitForFrame()
{
  instance_check();

  _swapReady = true;

  video_sync();
}

/*
 * @brief Retrieve pointer to frame buffer lines array
 */
uint8_t** ESP_8_BIT_composite::getFrameBufferLines()
{
  instance_check();

  return _backBuffer;
}

/*
 * @brief Number of frames sent to screen
 */
uint32_t ESP_8_BIT_composite::getRenderedFrameCount()
{
  return _frame_counter;
}

/*
 * @brief Number of buffer swaps performed
 */
uint32_t ESP_8_BIT_composite::getBufferSwapCount()
{
  return _swap_counter;
}

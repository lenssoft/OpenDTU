<template>
    <BasePage :title="$t('sunspecadmin.SunSpecSettings')" :isLoading="dataLoading">
        <BootstrapAlert :show="true" variant="warning">
            {{ $t('sunspecadmin.ExperimentalHint') }}
        </BootstrapAlert>

        <BootstrapAlert
            v-model="alert.show"
            dismissible
            :variant="alert.type"
            :auto-dismiss="alert.type != 'success' ? 0 : 5000"
        >
            {{ alert.message }}
        </BootstrapAlert>

        <form @submit="saveSunSpecConfig">
            <CardElement :text="$t('sunspecadmin.SunSpecConfiguration')" textVariant="text-bg-primary">
                <InputElement
                    :label="$t('sunspecadmin.EnableSunSpec')"
                    v-model="sunspecConfig.sunspec_enabled"
                    type="checkbox"
                    wide
                />

                <template v-if="sunspecConfig.sunspec_enabled">
                    <InputElement
                        :label="$t('sunspecadmin.DeviceName')"
                        v-model="sunspecConfig.sunspec_device_name"
                        type="text"
                        maxlength="31"
                        :placeholder="$t('sunspecadmin.DeviceNameHint')"
                    />

                    <InputElement
                        :label="$t('sunspecadmin.EnablePowerLimit')"
                        v-model="sunspecConfig.sunspec_power_limit_enabled"
                        type="checkbox"
                        wide
                    />

                    <div v-if="sunspecConfig.sunspec_power_limit_enabled" class="alert alert-info mt-2 mb-0" role="alert">
                        {{ $t('sunspecadmin.PowerLimitHint') }}
                    </div>
                </template>
            </CardElement>

            <CardElement
                v-if="sunspecConfig.sunspec_enabled"
                :text="$t('sunspecadmin.ConnectionInfo')"
                textVariant="text-bg-secondary"
                add-space
            >
                <div class="row mb-2">
                    <div class="col-sm-4 col-form-label">{{ $t('sunspecadmin.Protocol') }}</div>
                    <div class="col-sm-8 col-form-label fw-bold">Modbus TCP</div>
                </div>
                <div class="row mb-2">
                    <div class="col-sm-4 col-form-label">{{ $t('sunspecadmin.Port') }}</div>
                    <div class="col-sm-8 col-form-label fw-bold">502</div>
                </div>
                <div class="row mb-2">
                    <div class="col-sm-4 col-form-label">{{ $t('sunspecadmin.UnitId') }}</div>
                    <div class="col-sm-8 col-form-label fw-bold">126</div>
                </div>
                <div class="row mb-2">
                    <div class="col-sm-4 col-form-label">{{ $t('sunspecadmin.Models') }}</div>
                    <div class="col-sm-8 col-form-label fw-bold">1, 101, 120, 123</div>
                </div>
            </CardElement>

            <FormFooter @reload="getSunSpecConfig" />
        </form>
    </BasePage>
</template>

<script lang="ts">
import BasePage from '@/components/BasePage.vue';
import BootstrapAlert from '@/components/BootstrapAlert.vue';
import CardElement from '@/components/CardElement.vue';
import FormFooter from '@/components/FormFooter.vue';
import InputElement from '@/components/InputElement.vue';
import type { AlertResponse } from '@/types/AlertResponse';
import type { SunSpecConfig } from '@/types/SunSpecConfig';
import { authHeader, handleResponse } from '@/utils/authentication';
import { defineComponent } from 'vue';

export default defineComponent({
    components: {
        BasePage,
        BootstrapAlert,
        CardElement,
        FormFooter,
        InputElement,
    },
    data() {
        return {
            dataLoading: true,
            sunspecConfig: {} as SunSpecConfig,
            alert: {} as AlertResponse,
        };
    },
    created() {
        this.getSunSpecConfig();
    },
    methods: {
        getSunSpecConfig() {
            this.dataLoading = true;
            fetch('/api/sunspec/config', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.sunspecConfig = data;
                    this.dataLoading = false;
                });
        },
        saveSunSpecConfig(e: Event) {
            e.preventDefault();

            const formData = new FormData();
            formData.append('data', JSON.stringify(this.sunspecConfig));

            fetch('/api/sunspec/config', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((response) => {
                    this.alert.message = this.$t('apiresponse.' + response.code, response.param);
                    this.alert.type = response.type;
                    this.alert.show = true;
                });
        },
    },
});
</script>

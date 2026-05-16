import { createEffect, createSignal, Show, type Component } from 'solid-js';
import { currentLocale, t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { CollapsibleConfigBlock, StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { pushToast } from '../state/toast';

type BasicForm = {
  wifi_ssid: string;
  wifi_password: string;
  wifi_proxy_enabled: string;
  wifi_proxy_address: string;
  wifi_proxy_port: string;
  time_timezone: string;
  obs_endpoint: string;
  obs_bucket: string;
  obs_access_key: string;
  obs_secret_key: string;
  obs_security_token: string;
  xfyun_app_id: string;
  xfyun_api_key: string;
  xfyun_api_secret: string;
};

export const BasicPage: Component = () => {
  const tab = createConfigTab<BasicForm>({
    tab: 'basic',
    groups: ['wifi', 'time', 'obs', 'speech'],
    toForm: (config: Partial<AppConfig>) => ({
      wifi_ssid: config.wifi_ssid ?? '',
      wifi_password: config.wifi_password ?? '',
      wifi_proxy_enabled: config.wifi_proxy_enabled ?? '0',
      wifi_proxy_address: config.wifi_proxy_address ?? '',
      wifi_proxy_port: config.wifi_proxy_port ?? '',
      time_timezone: config.time_timezone ?? '',
      obs_endpoint: config.obs_endpoint ?? '',
      obs_bucket: config.obs_bucket ?? '',
      obs_access_key: config.obs_access_key ?? '',
      obs_secret_key: config.obs_secret_key ?? '',
      obs_security_token: config.obs_security_token ?? '',
      xfyun_app_id: config.xfyun_app_id ?? '',
      xfyun_api_key: config.xfyun_api_key ?? '',
      xfyun_api_secret: config.xfyun_api_secret ?? '',
    }),
    fromForm: (form) => ({
      wifi_ssid: form.wifi_ssid.trim(),
      wifi_password: form.wifi_password,
      wifi_proxy_enabled: form.wifi_proxy_enabled === '1' ? '1' : '0',
      wifi_proxy_address: form.wifi_proxy_address.trim(),
      wifi_proxy_port: form.wifi_proxy_port.trim(),
      time_timezone: form.time_timezone.trim(),
      obs_endpoint: form.obs_endpoint.trim(),
      obs_bucket: form.obs_bucket.trim(),
      obs_access_key: form.obs_access_key.trim(),
      obs_secret_key: form.obs_secret_key.trim(),
      obs_security_token: form.obs_security_token.trim(),
      xfyun_app_id: form.xfyun_app_id.trim(),
      xfyun_api_key: form.xfyun_api_key.trim(),
      xfyun_api_secret: form.xfyun_api_secret.trim(),
    }),
  });
  const [validationError, setValidationError] = createSignal<string | null>(null);

  createEffect(() => {
    void tab.form.wifi_ssid;
    void tab.form.wifi_password;
    void tab.form.wifi_proxy_enabled;
    void tab.form.wifi_proxy_address;
    void tab.form.wifi_proxy_port;
    void tab.form.obs_endpoint;
    void tab.form.obs_bucket;
    void tab.form.obs_access_key;
    void tab.form.obs_secret_key;
    void tab.form.obs_security_token;
    void tab.form.xfyun_app_id;
    void tab.form.xfyun_api_key;
    void tab.form.xfyun_api_secret;
    setValidationError(null);
  });

  const handleSave = async () => {
    const wifiSsid = tab.form.wifi_ssid.trim();
    const wifiPassword = tab.form.wifi_password;

    if (!wifiSsid) {
      const message = t('wifiValidationSsidRequired') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    if (wifiPassword.length > 0 && wifiPassword.length < 8) {
      const message = t('wifiValidationPasswordLength') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }
    const proxyAddress = tab.form.wifi_proxy_address.trim();
    const proxyPort = tab.form.wifi_proxy_port.trim();
    const proxyEnabled = tab.form.wifi_proxy_enabled === '1';
    if ((proxyAddress && !proxyPort) || (!proxyAddress && proxyPort)) {
      const message = t('wifiValidationProxyRequired') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }
    if (proxyPort && !/^([1-9][0-9]{0,4})$/.test(proxyPort)) {
      const message = t('wifiValidationProxyPort') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }
    if (proxyPort) {
      const parsedPort = Number(proxyPort);
      if (!Number.isInteger(parsedPort) || parsedPort < 1 || parsedPort > 65535) {
        const message = t('wifiValidationProxyPort') as string;
        setValidationError(message);
        pushToast(message, 'error', 5000);
        return;
      }
    }
    if (proxyEnabled && (!proxyAddress || !proxyPort)) {
      const message = t('wifiValidationProxyRequired') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    const obsEndpoint = tab.form.obs_endpoint.trim();
    const obsBucket = tab.form.obs_bucket.trim();
    const obsAk = tab.form.obs_access_key.trim();
    const obsSk = tab.form.obs_secret_key.trim();
    const hasObsSecret = Boolean(obsAk || obsSk || tab.form.obs_security_token.trim());
    if (hasObsSecret && (!obsEndpoint || !obsBucket)) {
      const message = t('obsValidationEndpointBucket') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }
    if ((obsAk && !obsSk) || (!obsAk && obsSk)) {
      const message = t('obsValidationAkSk') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    const xfyunFilled = [
      tab.form.xfyun_app_id.trim(),
      tab.form.xfyun_api_key.trim(),
      tab.form.xfyun_api_secret.trim(),
    ].filter(Boolean).length;
    if (xfyunFilled > 0 && xfyunFilled < 3) {
      const message = t('xfyunValidationComplete') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    await tab.save();
  };

  const timezoneHint = () =>
    currentLocale() === 'zh-cn' ? (
      <>
        仅接受 POSIX TZ 字符串，符号与日常 UTC 表示相反。北京时间（UTC+8）应写作
        {' '}
        "CST-8"
        ，纽约（UTC-5）写作 "EST5"。可在
        <a
          href="https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv"
          target="_blank"
          rel="noopener noreferrer"
          class="underline underline-offset-2 hover:text-[var(--color-text-primary)]"
        >
          此表格
        </a>
        查阅 IANA 时区与 POSIX 表达转换关系。
      </>
    ) : (
      (t('timezoneHelp') as string)
    );

  return (
    <TabShell>
      <PageHeader
        title={t('navBasic') as string}
        description={t('restartHint') as string}
      />
      <Show when={validationError() ?? tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={validationError() ?? tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionWifi') as string}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              label={t('wifiSsid')}
              autocomplete="off"
              value={tab.form.wifi_ssid}
              onInput={(event) => tab.setForm('wifi_ssid', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('wifiPassword')}
              autocomplete="new-password"
              value={tab.form.wifi_password}
              onInput={(event) => tab.setForm('wifi_password', event.currentTarget.value)}
            />
          </div>
        </StaticConfigBlock>
        <CollapsibleConfigBlock title={t('sectionWifiAdvanced') as string} defaultOpen={false}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <label class="sm:col-span-2 flex items-center gap-2 rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)] px-3 py-2 text-sm text-[var(--color-text-primary)]">
              <input
                type="checkbox"
                class="h-4 w-4 accent-[var(--color-accent)]"
                checked={tab.form.wifi_proxy_enabled === '1'}
                onChange={(event) => tab.setForm('wifi_proxy_enabled', event.currentTarget.checked ? '1' : '0')}
              />
              <span>{t('wifiProxyEnabled') as string}</span>
            </label>
            <TextInput
              label={t('wifiProxyAddress')}
              placeholder="192.168.1.100"
              value={tab.form.wifi_proxy_address}
              onInput={(event) => tab.setForm('wifi_proxy_address', event.currentTarget.value)}
            />
            <TextInput
              type="number"
              min="1"
              max="65535"
              inputmode="numeric"
              label={t('wifiProxyPort')}
              placeholder="7890"
              value={tab.form.wifi_proxy_port}
              onInput={(event) => tab.setForm('wifi_proxy_port', event.currentTarget.value)}
            />
          </div>
        </CollapsibleConfigBlock>
        <CollapsibleConfigBlock title={t('sectionObs') as string} defaultOpen={false}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              full
              label={t('obsEndpoint')}
              placeholder="obs.cn-north-4.myhuaweicloud.com"
              value={tab.form.obs_endpoint}
              onInput={(event) => tab.setForm('obs_endpoint', event.currentTarget.value)}
            />
            <TextInput
              label={t('obsBucket')}
              placeholder="your-bucket"
              value={tab.form.obs_bucket}
              onInput={(event) => tab.setForm('obs_bucket', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('obsAccessKey')}
              value={tab.form.obs_access_key}
              onInput={(event) => tab.setForm('obs_access_key', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('obsSecretKey')}
              value={tab.form.obs_secret_key}
              onInput={(event) => tab.setForm('obs_secret_key', event.currentTarget.value)}
            />
            <TextInput
              full
              type="password"
              label={t('obsSecurityToken')}
              hint={t('obsSecurityTokenHint')}
              value={tab.form.obs_security_token}
              onInput={(event) => tab.setForm('obs_security_token', event.currentTarget.value)}
            />
          </div>
        </CollapsibleConfigBlock>
        <CollapsibleConfigBlock title={t('sectionXfyunSpeech') as string} defaultOpen={false}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              label={t('xfyunAppId')}
              value={tab.form.xfyun_app_id}
              onInput={(event) => tab.setForm('xfyun_app_id', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('xfyunApiKey')}
              value={tab.form.xfyun_api_key}
              onInput={(event) => tab.setForm('xfyun_api_key', event.currentTarget.value)}
            />
            <TextInput
              full
              type="password"
              label={t('xfyunApiSecret')}
              hint={t('xfyunSpeechHint')}
              value={tab.form.xfyun_api_secret}
              onInput={(event) => tab.setForm('xfyun_api_secret', event.currentTarget.value)}
            />
          </div>
        </CollapsibleConfigBlock>
        <CollapsibleConfigBlock title={t('sectionAdvanced') as string} defaultOpen={false}>
          <div class="pt-2">
            <TextInput
              full
              label={t('timezone')}
              placeholder={t('timezonePlaceholder') as string}
              hint={timezoneHint()}
              value={tab.form.time_timezone}
              onInput={(event) => tab.setForm('time_timezone', event.currentTarget.value)}
            />
          </div>
        </CollapsibleConfigBlock>
      </div>
      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => handleSave().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
    </TabShell>
  );
};

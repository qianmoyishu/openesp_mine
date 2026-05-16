import { Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';

type SearchForm = {
  search_brave_key: string;
  search_tavily_key: string;
};

export const SearchPage: Component = () => {
  const tab = createConfigTab<SearchForm>({
    tab: 'search',
    groups: ['search'],
    toForm: (config: Partial<AppConfig>) => ({
      search_brave_key: config.search_brave_key ?? '',
      search_tavily_key: config.search_tavily_key ?? '',
    }),
    fromForm: (form) => ({
      search_brave_key: form.search_brave_key.trim(),
      search_tavily_key: form.search_tavily_key.trim(),
    }),
  });

  return (
    <TabShell>
      <PageHeader
        title={t('navSearch') as string}
        description={t('searchNote') as string}
      />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionSearch') as string}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              type="password"
              label={t('searchBraveKey')}
              value={tab.form.search_brave_key}
              onInput={(event) => tab.setForm('search_brave_key', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('searchTavilyKey')}
              value={tab.form.search_tavily_key}
              onInput={(event) => tab.setForm('search_tavily_key', event.currentTarget.value)}
            />
          </div>
        </StaticConfigBlock>
      </div>
      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => tab.save().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
    </TabShell>
  );
};

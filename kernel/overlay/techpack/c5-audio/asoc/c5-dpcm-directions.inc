/* SPDX-License-Identifier: GPL-2.0-only */
/* C5's 3.10 core derived dynamic/BE directions from the bound CPU DAI.
 * Native 4.9 consumes explicit link flags. Its card probe runs after DAI
 * binding and before stream construction, so preserve the old contract here
 * without changing core code, link IDs, callbacks or hardware capabilities.
 */
static int c5_dpcm_apply_cpu_directions(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd;
	unsigned int count = 0;

	list_for_each_entry(rtd, &card->rtd_list, list) {
		if (!rtd->dai_link)
			return -EINVAL;
		if (!(rtd->dai_link->dynamic || rtd->dai_link->no_pcm))
			continue;
		if (!rtd->cpu_dai || !rtd->cpu_dai->driver)
			return -EINVAL;
	}
	list_for_each_entry(rtd, &card->rtd_list, list) {
		if (!(rtd->dai_link->dynamic || rtd->dai_link->no_pcm))
			continue;
		rtd->dai_link->dpcm_playback =
			!!rtd->cpu_dai->driver->playback.channels_min;
		rtd->dai_link->dpcm_capture =
			!!rtd->cpu_dai->driver->capture.channels_min;
		count++;
	}
	pr_info("C5 audio DPCM directions restored from bound CPU DAIs: %u links\n",
		count);
	return 0;
}

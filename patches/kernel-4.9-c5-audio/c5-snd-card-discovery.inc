/* SPDX-License-Identifier: Apache-2.0 */
/* Retain the old aggregate sleep budget, but revisit asynchronously ready cards. */
int audio_extn_utils_get_snd_card_num(void)
{
    int round, card;
    const int rounds = MAX_SND_CARD * RETRY_NUMBER;

    for (round = 0; round < rounds; round++) {
        for (card = 0; card < MAX_SND_CARD; card++) {
            struct mixer *mixer = mixer_open(card);
            char *name;
            void *hw_info;

            if (!mixer)
                continue;
            name = strdup(mixer_get_name(mixer));
            if (!name) {
                mixer_close(mixer);
                ALOGE("%s: cannot allocate sound card name", __func__);
                return -1;
            }
            hw_info = hw_info_init(name);
            free(name);
            mixer_close(mixer);
            if (hw_info) {
                hw_info_deinit(hw_info);
                ALOGD("%s: Opened sound card:%d", __func__, card);
                return card;
            }
        }
        if (round + 1 < rounds)
            usleep(RETRY_US);
    }
    ALOGE("%s: Unable to find correct sound card, aborting.", __func__);
    return -1;
}

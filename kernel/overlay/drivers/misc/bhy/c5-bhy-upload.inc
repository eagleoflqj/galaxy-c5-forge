/* GPL-2.0: bounds and byte-order helper for the original C5 RAM patch stream. */
static int c5_bhy_prepare_upload(unsigned char *data, size_t length,
                                 size_t remaining)
{
    size_t i;
    unsigned char tmp;

    if (!data || !length || length > 64 || length > remaining ||
        (length & 3))
        return -EINVAL;
    for (i = 0; i < length; i += 4) {
        tmp = data[i];
        data[i] = data[i + 3];
        data[i + 3] = tmp;
        tmp = data[i + 1];
        data[i + 1] = data[i + 2];
        data[i + 2] = tmp;
    }
    return 0;
}

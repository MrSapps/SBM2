/* Unit tests for the video overlays (port/overlay.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overlay.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t wram[0x20000];

/* A new stage drops the last one's move: the 2-1 intro showed "do boss
 * 8,4" (1-8's) and the ending "survive (boss 8,7 ...)" (5-8's). */
static void test_now(const char *dir)
{
    printf("now\n");
    char path[512];
    snprintf(path, sizeof path, "%s/ov_test.ov", dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "W 10 boss 8,4 (get in place): walk to (6,10)\n");
    fprintf(f, "W 40 kill 3,3: bomb at (3,4), cover (1,1)\n");
    fclose(f);
    CHECK(ov_load(path) == 2);
    remove(path);
    wram[0x1A] = 0x17;
    ov_frame(wram, 10, NULL, 0, NULL, 0, NULL);
    CHECK(!strcmp(ov_now(), "boss 8,4 (get in place): walk to (6,10)"));
    wram[0x1A] = 0x20;  /* 2-1 starts: no move of its own yet */
    ov_frame(wram, 20, NULL, 0, NULL, 0, NULL);
    CHECK(!strcmp(ov_now(), "stage start"));
    ov_frame(wram, 40, NULL, 0, NULL, 0, NULL);
    CHECK(!strncmp(ov_now(), "kill 3,3", 8));
    wram[0x1A] = 0x60;
    ov_frame(wram, 50, NULL, 0, NULL, 0, NULL);
    CHECK(strstr(ov_now(), "ending") != NULL);
}

/* The futures grid fills the panel: across its whole width or down its
 * whole height (it was laid out in 752 of the panel's 792 px). */
static void test_grid(void)
{
    printf("grid\n");
    int W = ov_panel_w(), H = 718;
    for (int n = 1; n <= 8; n++) {
        int cols, ww, wh, gap;
        ov_grid(n, W, H, &cols, &ww, &wh, &gap);
        int rows = (n + cols - 1) / cols;
        int usedw = cols * ww + (cols - 1) * gap, usedh = rows * wh + (rows - 1) * gap;
        CHECK(usedw <= W && usedh <= H);
        if (usedw < W * 97 / 100 && usedh < H * 97 / 100)
            printf("  FAIL %d windows: %dx%d of %dx%d\n", n, usedw, usedh, W, H), fails++;
        checks++;
    }
}

int main(int argc, char **argv)
{
    test_now(argc > 1 ? argv[1] : ".");
    test_grid();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}

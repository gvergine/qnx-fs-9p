/* lib9p id pool tests. */
#include "lib9p/p9.h"
#include "test.h"

static void test_basic(void)
{
    uint32_t words[P9_IDPOOL_WORDS(33)];
    p9_idpool_t p;
    uint32_t id;

    CHECK_EQ(p9_idpool_init(&p, words, 0), P9_E_INVAL);
    CHECK_EQ(p9_idpool_init(&p, words, 33), P9_OK);
    CHECK_EQ(p9_idpool_used(&p), 0);

    for (uint32_t i = 0; i < 3; i++) {
        CHECK_EQ(p9_idpool_alloc(&p, &id), P9_OK);
        CHECK_EQ(id, i);
    }
    CHECK(p9_idpool_in_use(&p, 1));
    CHECK_EQ(p9_idpool_free(&p, 1), P9_OK);
    CHECK(!p9_idpool_in_use(&p, 1));
    CHECK_EQ(p9_idpool_free(&p, 1), P9_E_INVAL);  /* double free */
    CHECK_EQ(p9_idpool_free(&p, 33), P9_E_INVAL); /* out of range */
    CHECK(!p9_idpool_in_use(&p, 1000));

    /* Round-robin: the freed id 1 is not handed out next. */
    CHECK_EQ(p9_idpool_alloc(&p, &id), P9_OK);
    CHECK_EQ(id, 3);

    /* Fill up (ids 4..32 plus the freed 1), then exhaustion. */
    uint32_t got = 0;
    while (p9_idpool_alloc(&p, &id) == P9_OK)
        got++;
    CHECK_EQ(got, 30);
    CHECK_EQ(p9_idpool_used(&p), 33);
    CHECK_EQ(p9_idpool_alloc(&p, &id), P9_E_EXHAUSTED);

    /* After wrap-around the search finds a freed id anywhere. */
    CHECK_EQ(p9_idpool_free(&p, 20), P9_OK);
    CHECK_EQ(p9_idpool_alloc(&p, &id), P9_OK);
    CHECK_EQ(id, 20);
}

static void test_tag_range(void)
{
    /* A tag pool of 0xFFFF ids never hands out P9_NOTAG. */
    static uint32_t words[P9_IDPOOL_WORDS(0xFFFFu)];
    p9_idpool_t p;
    uint32_t id = 0, max = 0;
    CHECK_EQ(p9_idpool_init(&p, words, 0xFFFFu), P9_OK);
    while (p9_idpool_alloc(&p, &id) == P9_OK)
        if (id > max)
            max = id;
    CHECK_EQ(max, 0xFFFEu);
    CHECK_EQ(p9_idpool_used(&p), 0xFFFFu);

    uint32_t w[1];
    CHECK_EQ(p9_idpool_init(&p, w, 0xFFFFFFFFu), P9_E_INVAL); /* would include NOFID */
}

int main(void)
{
    test_basic();
    test_tag_range();
    return TEST_RESULT();
}

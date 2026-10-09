/*
 * Entry point for `make link-check`: the game starts in NitroMain, as on the
 * DS. Linking from here pulls in everything the game can reach, so the
 * linker lists every symbol that is still missing.
 */
void NitroMain(void);

int main(void)
{
    NitroMain();
    return 0;
}

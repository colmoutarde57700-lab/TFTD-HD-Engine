#ifdef __MINGW32__
extern "C" int SDL_main(int argc, char *argv[]);
int main(int argc, char *argv[])
{
    return SDL_main(argc, argv);
}
#endif

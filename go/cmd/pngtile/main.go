package main

import (
	"fmt"
	"github.com/qmsk/pngtile/go"
	"github.com/urfave/cli"
	"log"
	"os"
)

type Options struct {
	Debug      bool
	Quiet      bool
	Update     bool
	Background string
}

func (options Options) imageParams() (pngtile.ImageParams, error) {
	var imageParams pngtile.ImageParams

	if options.Background != "" {
		var backgroundPixel pngtile.ImagePixel

		_, err := fmt.Sscanf(options.Background, "%x%x%x%x",
			&backgroundPixel[0],
			&backgroundPixel[1],
			&backgroundPixel[2],
			&backgroundPixel[3],
		)

		if err != nil {
			return imageParams, fmt.Errorf("Invalid --background=%s: %v", options.Background, err)
		}

		imageParams.BackgroundPixel = &backgroundPixel
	}

	return imageParams, nil
}

func (options Options) run(path string) error {
	return pngtile.WithImage(path, pngtile.OPEN_UPDATE, func(image *pngtile.Image) error {
		if cacheStatus, err := image.Status(); err != nil {
			return err
		} else if cacheStatus != pngtile.CACHE_FRESH || options.Update {
			log.Printf("%s: cache update (status %v)", path, cacheStatus)

			if imageParams, err := options.imageParams(); err != nil {
				return err
			} else if err := image.Update(imageParams); err != nil {
				return err
			}

		} else {
			log.Printf("%s: cache fresh", path)

			if err := image.Open(); err != nil {
				return err
			}
		}

		if info, err := image.Info(); err != nil {
			return err
		} else {
			fmt.Printf("%s:\n", path)
			fmt.Printf("\tImage: %dx%d@%d\n", info.ImageWidth, info.ImageHeight, info.ImageBPP)
			fmt.Printf("\tImage mtime=%v bytes=%d\n", info.ImageModifiedTime, info.ImageBytes)
			fmt.Printf("\tCache mtime=%v bytes=%d version=%d blocks=%d\n", info.CacheModifiedTime, info.CacheBytes, info.CacheVersion, info.CacheBlocks)
		}

		return nil
	})
}

func main() {
	var options Options
	var app = cli.NewApp()

	app.Name = "pngtile"
	app.Usage = "Update pngtile caches for tile rendering"
	app.ArgsUsage = "[FILE ...]"
	app.Flags = []cli.Flag{
		cli.BoolFlag{
			Name:        "debug",
			Usage:       "Enable debug logging",
			Destination: &options.Debug,
		},
		cli.BoolFlag{
			Name:        "quiet",
			Usage:       "Disable warn logging",
			Destination: &options.Quiet,
		},

		cli.StringFlag{
			Name:        "background",
			Usage:       "Hexadecimal [1..4]uint8 pixel value",
			Destination: &options.Background,
		},
		cli.BoolFlag{
			Name:        "update",
			Usage:       "Force cache udpate",
			Destination: &options.Update,
		},
	}
	app.Before = func(c *cli.Context) error {
		pngtile.LogDebug(options.Debug)
		pngtile.LogWarn(!options.Quiet)

		return nil
	}
	app.Action = func(c *cli.Context) error {
		for _, arg := range c.Args() {
			if err := options.run(arg); err != nil {
				return fmt.Errorf("%s: %s", arg, err)
			}
		}
		return nil
	}

	if err := app.Run(os.Args); err != nil {
		log.Fatalf("%v", err)
	}
}

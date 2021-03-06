package main

import (
	"fmt"
	"github.com/qmsk/pngtile/go"
	"github.com/urfave/cli"
	"io/ioutil"
	"log"
	"math/rand"
	"os"
	"time"
)

type Options struct {
	Debug bool
	Quiet bool

	Recursive bool
	Update    bool

	Background string
	TileOut    string
	TileParams pngtile.TileParams
	TileRandom bool
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
	log.Printf("%s", path)

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

			if options.TileRandom {
				r := rand.New(rand.NewSource(time.Now().Unix()))

				options.TileParams.X = uint(r.Intn(int(info.ImageWidth)))
				options.TileParams.Y = uint(r.Intn(int(info.ImageHeight)))
			}
		}

		if options.TileOut != "" {
			if tileData, err := image.Tile(options.TileParams); err != nil {
				return fmt.Errorf("Render --tile: %v", err)
			} else if err := ioutil.WriteFile(options.TileOut, tileData, 0644); err != nil {
				return fmt.Errorf("Write --tile-out=%s: %v", options.TileOut, err)
			} else {
				log.Printf("%s: render %dx%d tile at %dx%d@%d to %s", path,
					options.TileParams.Width,
					options.TileParams.Height,
					options.TileParams.X,
					options.TileParams.Y,
					options.TileParams.Zoom,
					options.TileOut,
				)
			}
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

		cli.BoolFlag{
			Name:        "recursive",
			Usage:       "scan directory recursively for image files",
			Destination: &options.Recursive,
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

		cli.StringFlag{
			Name:        "tile-out",
			Usage:       "Render tile to file",
			Destination: &options.TileOut,
		},
		cli.UintFlag{
			Name:        "tile-width",
			Usage:       "Tile width pixels",
			Value:       256,
			Destination: &options.TileParams.Width,
		},
		cli.UintFlag{
			Name:        "tile-height",
			Usage:       "Tile height pixels",
			Value:       256,
			Destination: &options.TileParams.Height,
		},
		cli.UintFlag{
			Name:        "tile-x",
			Usage:       "Tile X pixels",
			Value:       0,
			Destination: &options.TileParams.X,
		},
		cli.UintFlag{
			Name:        "tile-y",
			Usage:       "Tile Y pixels",
			Value:       0,
			Destination: &options.TileParams.Y,
		},
		cli.IntFlag{
			Name:        "tile-zoom",
			Usage:       "Tile zoom factor (-/0/+)",
			Value:       0,
			Destination: &options.TileParams.Zoom,
		},
		cli.BoolFlag{
			Name:        "tile-random",
			Usage:       "Randomize tile X/Y",
			Destination: &options.TileRandom,
		},
	}
	app.Before = func(c *cli.Context) error {
		pngtile.LogDebug(options.Debug)
		pngtile.LogWarn(!options.Quiet)

		return nil
	}
	app.Action = func(c *cli.Context) error {
		for _, arg := range c.Args() {
			if options.Recursive {
				log.Printf("%s...", arg)

				if scanImages, err := pngtile.Scan(arg, pngtile.ScanOptions{IncludeCached: false}); err != nil {
					return fmt.Errorf("scan %s: %v", arg, err)
				} else {
					for _, scanImage := range scanImages {
						if err := options.run(scanImage.Path); err != nil {
							return fmt.Errorf("%s: %s", scanImage.Path, err)
						}
					}
				}
			} else {
				if err := options.run(arg); err != nil {
					return fmt.Errorf("%s: %s", arg, err)
				}
			}
		}
		return nil
	}

	if err := app.Run(os.Args); err != nil {
		log.Fatalf("%v", err)
	}
}

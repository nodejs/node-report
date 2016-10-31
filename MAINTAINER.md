# Instructions for maintainers of the nodereport project

## Releasing to www.npmjs.com

The nodereport project is published as an NPM native module here: https://www.npmjs.com/package/nodereport

On each release to www.npmjs.com

 - update the version property in the package.json file, incrementing the major, minor and patch level as appropriate 
 - update the CHANGES.md file with a list of commits since last release
 - commit CHANGES.md and package.json to nodereport master branch
 - tag commit with an annotated tag
 - git checkout and npm publish the nodereport package

Suggested tooling is the slt-release script documented here: https://github.com/strongloop/strong-tools

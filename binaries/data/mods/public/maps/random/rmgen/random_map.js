/**
 * @file The RandomMap stores the elevation grid, terrain textures and entities that are exported to the engine.
 *
 * @param {Number} baseHeight - Initial elevation of the map
 * @param {String|Array} baseTerrain - One or more texture names
 */
function RandomMap(baseHeight, baseTerrain)
{
	log("Initializing map...");

	// Size must be 0 to 1024, divisible by patches
	this.size = g_MapSettings.Size;

	// Create name <-> id maps for textures
	this.nameToID = {};
	this.IDToName = [];

	// Texture 2D array
	this.texture = [];
	for (let x = 0; x < this.size; ++x)
	{
		this.texture[x] = new Uint16Array(this.size);

		for (let z = 0; z < this.size; ++z)
			this.texture[x][z] = this.getTextureID(
				typeof baseTerrain == "string" ? baseTerrain : pickRandom(baseTerrain));
	}

	// Create 2D arrays for terrain objects and areas
	this.terrainObjects = [];
	this.area = [];

	for (let i = 0; i < this.size; ++i)
	{
		// Area IDs
		this.area[i] = new Uint16Array(this.size);

		// Entities
		this.terrainObjects[i] = [];
		for (let j = 0; j < this.size; ++j)
			this.terrainObjects[i][j] = undefined;
	}

	// Create 2D array for heightmap
	let mapSize = this.size;
	if (!TILE_CENTERED_HEIGHT_MAP)
		++mapSize;

	this.height = [];
	for (let i = 0; i < mapSize; ++i)
	{
		this.height[i] = new Float32Array(mapSize);

		for (let j = 0; j < mapSize; ++j)
			this.height[i][j] = baseHeight;
	}

	// Array of Entities
	this.objects = [];

	// Array of integers
	this.tileClasses = [];

	this.areaID = 0;

	// Starting entity ID, arbitrary number to leave some space for player entities
	this.entityCount = 150;
}

/**
 * Returns the ID of a texture name.
 * Creates a new ID if there isn't one assigned yet.
 */
RandomMap.prototype.getTextureID = function(texture)
{
	if (texture in this.nameToID)
		return this.nameToID[texture];

	let id = this.IDToName.length;
	this.nameToID[texture] = id;
	this.IDToName[id] = texture;

	return id;
};

/**
 * Returns the next unused entityID.
 */
RandomMap.prototype.getEntityID = function()
{
	return this.entityCount++;
};

/**
 * Determines whether the given coordinates are within the given distance of the passable map area.
 * Should be used to restrict entity placement and path creation.
 */
RandomMap.prototype.validT = function(x, z, distance = 0)
{
	distance += MAP_BORDER_WIDTH;

	if (g_MapSettings.CircularMap)
	{
		let halfSize = Math.floor(this.size / 2);
		return Math.round(Math.euclidDistance2D(x, z, halfSize, halfSize)) < halfSize - distance - 1;
	}
	else
		return x >= distance && z >= distance && x < this.size - distance && z < this.size - distance;
};

/**
 * Determines whether the given coordinates are within the tile grid, passable or not.
 * Should be used to restrict texture painting.
 */
RandomMap.prototype.inMapBounds = function(position)
{
	return position.x >= 0 && position.y >= 0 && position.x < this.size && position.y < this.size;
};

/**
 * Determines whether the given coordinates are within the heightmap grid.
 * Should be used to restrict elevation changes.
 */
RandomMap.prototype.validH = function(x, z)
{
	if (x < 0 || z < 0)
		return false;
	if (TILE_CENTERED_HEIGHT_MAP)
		return x < this.size && z < this.size;
	return x <= this.size && z <= this.size;
};

/**
 * Tests if there is a tileclass with the given ID.
 */
RandomMap.prototype.validClass = function(tileClassID)
{
	return tileClassID >= 0 && tileClassID < this.tileClasses.length;
};

/**
 * Returns the name of the texture of the given tile.
 */
RandomMap.prototype.getTexture = function(x, z)
{
	if (!this.validT(x, z))
		throw new Error("getTexture: invalid tile position (" + x + ", " + z + ")");

	return this.IDToName[this.texture[x][z]];
};

/**
 * Paints the given texture on the given tile.
 */
RandomMap.prototype.setTexture = function(position, texture)
{
	if (position.x < 0 ||
	    position.y < 0 ||
	    position.x >= this.texture.length ||
	    position.y >= this.texture[position.x].length)
		throw new Error("setTexture: invalid tile position " + uneval(position));

	this.texture[position.x][position.y] = this.getTextureID(texture);
};

RandomMap.prototype.getHeight = function(position)
{
	if (!this.validH(position.x, position.y))
		throw new Error("getHeight: invalid vertex position " + uneval(position));

	return this.height[position.x][position.y];
};

RandomMap.prototype.setHeight = function(position, height)
{
	if (!this.validH(position.x, position.y))
		throw new Error("setHeight: invalid vertex position " + uneval(position));

	this.height[position.x][position.y] = height;
};

/**
 * Returns the Entity that was painted by a Terrain class on the given tile or undefined otherwise.
 */
RandomMap.prototype.getTerrainObject = function(x, z)
{
	if (!this.validT(x, z))
		throw new Error("getTerrainObject: invalid tile position (" + x + ", " + z + ")");

	return this.terrainObjects[x][z];
};

/**
 * Places the Entity on the given tile and allows to later replace it if the terrain was painted over.
 */
RandomMap.prototype.setTerrainObject = function(x, z, object)
{
	if (!this.validT(x, z))
		throw new Error("setTerrainObject: invalid tile position (" + x + ", " + z + ")");

	this.terrainObjects[x][z] = object;
};

/**
 * Adds the given Entity to the map at the location it defines.
 */
RandomMap.prototype.addObject = function(obj)
{
	this.objects.push(obj);
};

/**
 * Constructs a new Area object and informs the Map which points correspond to this area.
 */
RandomMap.prototype.createArea = function(points)
{
	let areaID = ++this.areaID;
	for (let p of points)
		this.area[p.x][p.y] = areaID;
	return new Area(points, areaID);
};

/**
 * Returns an unused tileclass ID.
 */
RandomMap.prototype.createTileClass = function()
{
	let newID = this.tileClasses.length;
	this.tileClasses.push(new TileClass(this.size, newID));
	return newID;
};

/**
 * Retrieve interpolated height for arbitrary coordinates within the heightmap grid.
 */
RandomMap.prototype.getExactHeight = function(x, z)
{
	let xi = Math.min(Math.floor(x), this.size);
	let zi = Math.min(Math.floor(z), this.size);
	let xf = x - xi;
	let zf = z - zi;

	let h00 = this.height[xi][zi];
	let h01 = this.height[xi][zi + 1];
	let h10 = this.height[xi + 1][zi];
	let h11 = this.height[xi + 1][zi + 1];

	return (1 - zf) * ((1 - xf) * h00 + xf * h10) + zf * ((1 - xf) * h01 + xf * h11);
};

// Converts from the tile centered height map to the corner based height map, used when TILE_CENTERED_HEIGHT_MAP = true
RandomMap.prototype.cornerHeight = function(x, z)
{
	let count = 0;
	let sumHeight = 0;

	for (let dir of [[-1, -1], [-1, 0], [0, -1], [0, 0]])
		if (this.validH(x + dir[0], z + dir[1]))
		{
			++count;
			sumHeight += this.height[x + dir[0]][z + dir[1]];
		}

	if (count == 0)
		return 0;

	return sumHeight / count;
};

RandomMap.prototype.getAdjacentPoints = function(position)
{
	let adjacentPositions = [];

	for (let x = -1; x <= 1; ++x)
		for (let z = -1; z <= 1; ++z)
			if (x || z )
			{
				let adjacentPos = Vector2D.add(position, new Vector2D(x, z)).round();
				if (this.inMapBounds(adjacentPos))
					adjacentPositions.push(adjacentPos);
			}

	return adjacentPositions;
}

/**
 * Returns the average height of adjacent tiles, helpful for smoothing.
 */
RandomMap.prototype.getAverageHeight = function(position)
{
	let adjacentPositions = this.getAdjacentPoints(position);
	if (!adjacentPositions.length)
		return 0;

	return adjacentPositions.reduce((totalHeight, pos) => totalHeight + this.getHeight(pos), 0) / adjacentPositions.length;
}

/**
 * Returns the steepness of the given location, defined as the average height difference of the adjacent tiles.
 */
RandomMap.prototype.getSlope = function(position)
{
	let adjacentPositions = this.getAdjacentPoints(position);
	if (!adjacentPositions.length)
		return 0;

	return adjacentPositions.reduce((totalSlope, adjacentPos) =>
		totalSlope + Math.abs(this.getHeight(adjacentPos) - this.getHeight(position)), 0) / adjacentPositions.length;
}

/**
 * Retrieve an array of all Entities placed on the map.
 */
RandomMap.prototype.exportEntityList = function()
{
	// Change rotation from simple 2d to 3d befor giving to engine
	for (let obj of this.objects)
		obj.rotation.y = Math.PI / 2 - obj.rotation.y;

	// Terrain objects e.g. trees
	for (let x = 0; x < this.size; ++x)
		for (let z = 0; z < this.size; ++z)
			if (this.terrainObjects[x][z])
				this.objects.push(this.terrainObjects[x][z]);

	log("Number of entities: " + this.objects.length);
	return this.objects;
};

/**
 * Convert the elevation grid to a one-dimensional array.
 */
RandomMap.prototype.exportHeightData = function()
{
	let heightmapSize = this.size + 1;
	let heightmap = new Uint16Array(Math.square(heightmapSize));

	for (let x = 0; x < heightmapSize; ++x)
		for (let z = 0; z < heightmapSize; ++z)
		{
			let currentHeight = TILE_CENTERED_HEIGHT_MAP ? this.cornerHeight(x, z) : this.height[x][z];

			// Correct height by SEA_LEVEL and prevent under/overflow in terrain data
			heightmap[z * heightmapSize + x] = Math.max(0, Math.min(0xFFFF, Math.floor((currentHeight + SEA_LEVEL) * HEIGHT_UNITS_PER_METRE)));
		}

	return heightmap;
};

/**
 * Assemble terrain textures in a one-dimensional array.
 */
RandomMap.prototype.exportTerrainTextures = function()
{
	let tileIndex = new Uint16Array(Math.square(this.size));
	let tilePriority = new Uint16Array(Math.square(this.size));

	for (let x = 0; x < this.size; ++x)
		for (let z = 0; z < this.size; ++z)
		{
			// TODO: For now just use the texture's index as priority, might want to do this another way
			tileIndex[z * this.size + x] = this.texture[x][z];
			tilePriority[z * this.size + x] = this.texture[x][z];
		}

	return {
		"index": tileIndex,
		"priority": tilePriority
	};
};

RandomMap.prototype.ExportMap = function()
{
	log("Saving map...");

	if (g_Environment.Water.WaterBody.Height === undefined)
		g_Environment.Water.WaterBody.Height = SEA_LEVEL - 0.1;

	Engine.ExportMap({
		"entities": this.exportEntityList(),
		"height": this.exportHeightData(),
		"seaLevel": SEA_LEVEL,
		"size": this.size,
		"textureNames": this.IDToName,
		"tileData": this.exportTerrainTextures(),
		"Camera": g_Camera,
		"Environment": g_Environment
	});
}
